// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "ttnn/run_operation.hpp"
#include "ttnn/common/constants.hpp"
#include "ttnn/decorators.hpp"
#include "device/transpose_op.hpp"
#include "ttnn/operations/data_movement/permute/permute.hpp"
#include "ttnn/operations/data_movement/transpose/transpose.hpp"
#include "ttnn/cpp/ttnn/operations/copy.hpp"
#include "ttnn/cpp/ttnn/operations/data_movement/pad/pad.hpp"
#include "ttnn/cpp/ttnn/operations/data_movement/slice/slice.hpp"

namespace ttnn::operations::data_movement {

namespace detail {

inline uint32_t get_estimated_size_of_cbs(const Tensor& input_tensor_a) {
    // Circular Buffer sizes:
    uint32_t element_size = input_tensor_a.element_size();
    uint32_t Wt = input_tensor_a.get_padded_shape()[-1] / tt::constants::TILE_WIDTH;
    uint32_t Ht = input_tensor_a.get_padded_shape()[-2] / tt::constants::TILE_HEIGHT;
    uint32_t HtWt = Ht * Wt;
    auto data_format = tt::tt_metal::datatype_to_dataformat_converter(input_tensor_a.get_dtype());
    uint32_t tile_size = tt::tt_metal::detail::TileSize(data_format);

    uint32_t cb_src0_size = 2 * Wt * tile_size;
    uint32_t cb_output_size = 2 * Ht * tile_size;
    uint32_t cb_im_size = Ht * Wt * tile_size;
    uint32_t cb_im2_size = Ht * tile_size;
    return cb_src0_size + cb_output_size + cb_im_size + cb_im2_size;
}

inline uint32_t get_max_l1_space(const Tensor& input_tensor_a) {
    tt::tt_metal::Device* device = input_tensor_a.device();
    const std::vector<uint32_t>& bank_ids =
        device->bank_ids_from_logical_core(BufferType::L1, *device->compute_cores_.begin());
    std::optional<uint64_t> lowest_address = allocator::lowest_occupied_l1_address(*device->allocator_, bank_ids[0]);
    uint32_t max_l1_space = lowest_address.has_value() ? lowest_address.value() : device->l1_size_per_core();
    max_l1_space = max_l1_space - device->get_base_allocator_addr(HalMemType::L1);
    return max_l1_space;
}

inline bool rm_enough_available_space(const Tensor& input_tensor_a) {
    uint32_t max_l1_space = get_max_l1_space(input_tensor_a);
    uint32_t estimated_size_of_cbs = get_estimated_size_of_cbs(input_tensor_a);
    return max_l1_space > estimated_size_of_cbs;
}

inline Tensor transpose_(
    const Tensor& a,
    TransposeOpDim transpose_dim,
    const MemoryConfig& output_mem_config,
    const std::optional<float>& pad_value) {
    bool tiled_only = false;
    constexpr uint32_t FACE_WIDTH =
        tt::constants::FACE_WIDTH;  // this is a highly restrictive constraint on the RM transpose_wh kernel, and with
                                    // all the other bugs/limitations we should rewrite it
    // use device->get_allocator_alignment when the it reflects the alignment of the buffer and doesn't just default to
    // DRAM
    auto BUFFER_ALIGNMENT = a.buffer()->buffer_type() == tt::tt_metal::BufferType::DRAM ? DRAM_ALIGNMENT : L1_ALIGNMENT;
    uint32_t W = a.get_padded_shape()[-1];
    uint32_t H = a.get_padded_shape()[-2];
    switch (transpose_dim) {
        case TransposeOpDim::HC:
            tiled_only = a.get_layout() == Layout::TILE;
            if ((!tiled_only) && ((W * a.element_size()) % BUFFER_ALIGNMENT != 0)) {  //
                tiled_only = true;
            }
            break;
        // bubble dim around to make it possible as these implementations don't have a kernel
        case TransposeOpDim::NH:
            return ttnn::permute(
                (const ttnn::Tensor)a, ttnn::SmallVector<int64_t>({2, 1, 0, 3}), output_mem_config, pad_value);
        case TransposeOpDim::NW:
            return ttnn::permute(
                (const ttnn::Tensor)a, ttnn::SmallVector<int64_t>({3, 1, 2, 0}), output_mem_config, pad_value);
        case TransposeOpDim::CW:
            return ttnn::permute(
                (const ttnn::Tensor)a, ttnn::SmallVector<int64_t>({0, 3, 2, 1}), output_mem_config, pad_value);
        case TransposeOpDim::CN:
            tiled_only = true;  // CN only has a tiled implementation at the moment
            break;
        case TransposeOpDim::WH:  // THIS NEEDS TO BE FIXED
            if (((W * a.element_size()) % FACE_WIDTH != 0) || ((H * a.element_size()) % FACE_WIDTH != 0)) {
                tiled_only = true;
            } else if (a.device()->arch() == tt::ARCH::GRAYSKULL) {
                tiled_only = a.shape()[-2] > 256;  // hangs right now past this dimension, #13660 will turn it from a
                                                   // hang into a PCC issue for GS and improve perf for WH
            } else if (
                !a.is_sharded() && a.layout() == Layout::ROW_MAJOR &&
                !rm_enough_available_space(
                    a)) {  // rm is L1 intensive, if it overflows we can do tiled which allocates much smaller CBs
                tiled_only = true;
            }
            break;
        default: break;
    }
    if (a.get_layout() == Layout::ROW_MAJOR) {
        // the assorted cases where only tiled works right now (HC with stick width constraint, WH with stick width
        // constraint, CN).
        if (tiled_only) {
            // convert to tiled
            Tensor b = ttnn::to_layout(a, Layout::TILE, std::nullopt, std::nullopt, (Device*)nullptr);
            // run the transpose.
            b = operation::run(Transpose{transpose_dim, output_mem_config, pad_value}, {b}).at(0);
            // back to original layout
            b = ttnn::to_layout(b, a.get_layout(), std::nullopt, std::nullopt, (Device*)nullptr);
            return b;
        }
        return operation::run(Transpose{transpose_dim, output_mem_config, pad_value}, {a}).at(0);
    } else {
        return operation::run(Transpose{transpose_dim, output_mem_config, pad_value}, {a}).at(0);
    }
}

ttnn::Tensor transpose_nd(
    const ttnn::Tensor& input_tensor,
    const uint32_t dim1,
    const uint32_t dim2,
    const std::optional<MemoryConfig>& memory_config_arg,
    const std::optional<float>& pad_value) {
    std::vector<int64_t> permutation;
    permutation.reserve(input_tensor.get_shape().rank());
    for (uint32_t i = 0; i < input_tensor.get_shape().rank(); ++i) {
        permutation.push_back(i);
    }
    std::swap(permutation[dim1], permutation[dim2]);
    return ttnn::permute(input_tensor, permutation, memory_config_arg, pad_value);
}

}  // namespace detail

ttnn::Tensor ExecuteTranspose::invoke(
    uint8_t queue_id,
    const ttnn::Tensor& input_tensor,
    const int64_t& dim1,
    const int64_t& dim2,
    const std::optional<MemoryConfig>& memory_config_arg,
    const std::optional<float>& pad_value) {
    uint32_t normalized_dim1 = input_tensor.get_shape().get_normalized_index(dim1);
    uint32_t normalized_dim2 = input_tensor.get_shape().get_normalized_index(dim2);

    Tensor input_unsqueezed = input_tensor;
    uint32_t initial_rank = input_tensor.get_logical_shape().rank();
    if (initial_rank < 4) {
        input_unsqueezed = ttnn::unsqueeze_to_4D(input_tensor);
        uint32_t rank_diff = 4 - initial_rank;
        normalized_dim1 += rank_diff;
        normalized_dim2 += rank_diff;
    } else if (initial_rank > 4) {
        return detail::transpose_nd(input_tensor, normalized_dim1, normalized_dim2, memory_config_arg, pad_value);
    }

    bool wh = (normalized_dim1 == 2 && normalized_dim2 == 3) || (normalized_dim2 == 2 && normalized_dim1 == 3);
    bool cn = (normalized_dim1 == 0 && normalized_dim2 == 1) || (normalized_dim2 == 0 && normalized_dim1 == 1);
    bool bfloat8_supported = cn || wh;
    bool typecast =
        input_unsqueezed.get_dtype() == DataType::BFLOAT8_B and !bfloat8_supported and !input_unsqueezed.is_sharded();
    Tensor input_typecasted = typecast ? ttnn::typecast(input_unsqueezed, DataType::BFLOAT16) : input_unsqueezed;

    std::vector<Tensor> output_tensors = {Tensor(operation::get_workers_for_op_output({input_typecasted}))};
    operation::launch_with_autoformat(
        [normalized_dim1, normalized_dim2, memory_config_arg, pad_value](
            const std::vector<Tensor>& input_tensors,
            const std::vector<std::optional<const Tensor>>& optional_input_tensors,
            const std::vector<std::optional<Tensor>>& optional_output_tensors) mutable -> std::vector<Tensor> {
            auto& a = input_tensors.at(0);
            auto memory_config = memory_config_arg.value_or(a.memory_config());

            TT_FATAL(normalized_dim1 <= 3, "dimension has to be 0-3 only corresponding to N,C,H,W");
            TT_FATAL(normalized_dim2 <= 3, "dimension has to be 0-3 only corresponding to N,C,H,W");

            if ((normalized_dim1 == normalized_dim2) ||
                (a.get_padded_shape()[normalized_dim1] == 1 && a.get_padded_shape()[normalized_dim2] == 1)) {
                return {ttnn::operations::experimental::auto_format::AutoFormat::move_tensor_to_mem_config(
                    a, memory_config)};
            }

            if (normalized_dim1 > normalized_dim2) {
                std::swap(normalized_dim1, normalized_dim2);
            }

            TransposeOpDim transpose_dim = TransposeOpDim::NW;

            if (normalized_dim2 == 3 && normalized_dim1 == 0) {
                transpose_dim = TransposeOpDim::NW;
            } else if (normalized_dim2 == 3 && normalized_dim1 == 1) {
                transpose_dim = TransposeOpDim::CW;
            } else if (normalized_dim2 == 3 && normalized_dim1 == 2) {
                transpose_dim = TransposeOpDim::WH;
            } else if (normalized_dim2 == 2 && normalized_dim1 == 0) {
                transpose_dim = TransposeOpDim::NH;
            } else if (normalized_dim2 == 2 && normalized_dim1 == 1) {
                transpose_dim = TransposeOpDim::HC;
            } else if (normalized_dim2 == 1 && normalized_dim1 == 0) {
                transpose_dim = TransposeOpDim::CN;
            } else {
                TT_ASSERT(false, "Unsupported transpose dims");
            }
            return {detail::transpose_(a, transpose_dim, memory_config, pad_value)};
        },
        {input_typecasted},
        output_tensors);

    auto output = output_tensors.at(0);
    output = initial_rank < 4u ? ttnn::squeeze_from_4D(output, initial_rank) : output;
    return typecast ? ttnn::typecast(output, DataType::BFLOAT8_B) : output;
}

ttnn::Tensor ExecuteTranspose::invoke(
    const ttnn::Tensor& input_tensor,
    const int64_t& dim1,
    const int64_t& dim2,
    const std::optional<MemoryConfig>& memory_config,
    const std::optional<float>& pad_value) {
    return invoke(DefaultQueueId, input_tensor, dim1, dim2, memory_config, pad_value);
}

ttnn::Tensor ExecuteTranspose::invoke(
    const ttnn::Tensor& input_tensor, const int64_t& dim1, const int64_t& dim2, const std::optional<float>& pad_value) {
    return invoke(DefaultQueueId, input_tensor, dim1, dim2, std::nullopt, pad_value);
}

}  // namespace ttnn::operations::data_movement
