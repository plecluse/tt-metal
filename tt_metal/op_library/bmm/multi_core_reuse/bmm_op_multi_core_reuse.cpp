#include "tt_metal/op_library/bmm/bmm_op.hpp"

#include "tt_metal/host_api.hpp"
#include "common/constants.hpp"

using namespace tt::constants;
using namespace tt;

tt_metal::Program * create_program(
    tt_metal::Device *device,
    uint32_t single_tile_size,
    uint32_t num_cores_x,
    uint32_t M, uint32_t N, uint32_t K,
    uint32_t in0_block_w,
    uint32_t out_subblock_h, uint32_t out_subblock_w,
    uint32_t per_core_M, uint32_t per_core_N,
    uint32_t in0_dram_addr, uint32_t in1_dram_addr, uint32_t out_dram_addr
) {

    tt_metal::Program *program = new tt_metal::Program();

    uint32_t in0_block_tiles = per_core_M * in0_block_w;
    uint32_t in0_CB_size = in0_block_tiles * 2 * single_tile_size; // double buffer
    uint32_t in1_block_tiles = per_core_N * in0_block_w;
    uint32_t in1_CB_size = in1_block_tiles * 2 * single_tile_size; // double buffer
    uint32_t out_CB_tiles = per_core_M * per_core_N;
    uint32_t out_CB_size = out_CB_tiles * single_tile_size;
    TT_ASSERT(in0_CB_size <= 130*1024);
    TT_ASSERT(in1_CB_size <= 130*1024);
    TT_ASSERT(out_CB_size <= 540*1024);

    // Compute kernel compile time args
    uint32_t num_blocks = (K/in0_block_w);

    uint32_t in0_num_subblocks = (per_core_M/out_subblock_h);
    uint32_t in0_block_num_tiles = out_subblock_h*in0_block_w*in0_num_subblocks;
    uint32_t in0_subblock_num_tiles = out_subblock_h * in0_block_w;

    uint32_t in1_num_subblocks = (per_core_N/out_subblock_w);
    uint32_t in1_block_num_tiles = out_subblock_w*in0_block_w*in1_num_subblocks;
    uint32_t in1_per_core_w = out_subblock_w * in1_num_subblocks;

    uint32_t out_subblock_num_tiles = out_subblock_h*out_subblock_w;

    vector<uint32_t> compute_kernel_args = {
        in0_block_w, // in0_block_w
        in0_num_subblocks, // in0_num_subblocks
        in0_block_num_tiles, // in0_block_num_tiles
        in0_subblock_num_tiles, // in0_subblock_num_tiles

        in1_num_subblocks, // in1_num_subblocks
        in1_block_num_tiles, // in1_block_num_tiles
        in1_per_core_w, // in1_per_core_w

        num_blocks, // num_blocks

        out_subblock_h, // out_subblock_h
        out_subblock_w, // out_subblock_w
        out_subblock_num_tiles // out_subblock_num_tiles
    };

    uint32_t num_blocks_read = 0;
    for(int output_idx_y = 0; output_idx_y < M / per_core_M; output_idx_y++) {
        for(int output_idx_x = 0; output_idx_x < N / per_core_N; output_idx_x++) {
            int core_idx_x = num_blocks_read % num_cores_x;
            int core_idx_y = num_blocks_read / num_cores_x;
            tt_xy_pair core = {(std::size_t) core_idx_x, (std::size_t) core_idx_y};
            uint32_t l1_valid_address = 200 * 1024;

            uint32_t src0_cb_index = 0;
            uint32_t src0_cb_addr = l1_valid_address;
            l1_valid_address += in0_CB_size;
            uint32_t cb0_tiles = in0_block_tiles * 2; // double buffer
            auto cb_src0 = tt_metal::CreateCircularBuffer(
                program,
                device,
                src0_cb_index,
                core,
                cb0_tiles,
                cb0_tiles * single_tile_size,
                src0_cb_addr,
                tt::DataFormat::Float16_b
            );

            uint32_t src1_cb_index = 1;
            uint32_t src1_cb_addr = l1_valid_address;
            l1_valid_address += in1_CB_size;
            uint32_t cb1_tiles = in1_block_tiles * 2; // double buffer
            auto cb_src1 = tt_metal::CreateCircularBuffer(
                program,
                device,
                src1_cb_index,
                core,
                cb1_tiles,
                cb1_tiles * single_tile_size,
                src1_cb_addr,
                tt::DataFormat::Float16_b
            );

            uint32_t ouput_cb_index = 16; // output operands start at index 16
            uint32_t output_cb_addr = l1_valid_address;
            l1_valid_address += out_CB_size;
            auto cb_output = tt_metal::CreateCircularBuffer(
                program,
                device,
                ouput_cb_index,
                core,
                out_CB_tiles,
                out_CB_size,
                output_cb_addr,
                tt::DataFormat::Float16_b
            );

            uint32_t interm0_cb_index = 24;
            auto cb_interm0 = tt_metal::CreateCircularBuffer(
                program,
                device,
                interm0_cb_index,
                core,
                out_CB_tiles,
                out_CB_size,
                output_cb_addr,
                tt::DataFormat::Float16_b
            );

            TT_ASSERT(l1_valid_address < 1024 * 1024);

            // Create reader and writer kernels per core
            auto mm_reader_kernel = tt_metal::CreateDataMovementKernel(
                program,
                "kernels/dataflow/reader_matmul_tile_layout.cpp",
                core,
                tt_metal::DataMovementProcessor::RISCV_1,
                tt_metal::NOC::RISCV_1_default);

            auto unary_writer_kernel = tt_metal::CreateDataMovementKernel(
                program,
                "kernels/dataflow/writer_matmul_tile_layout.cpp",
                core,
                tt_metal::DataMovementProcessor::RISCV_0,
                tt_metal::NOC::RISCV_0_default);

            // Create compute kernel
            tt_metal::ComputeKernelArgs *mm_args = tt_metal::InitializeCompileTimeComputeKernelArgs(core, compute_kernel_args);
            bool fp32_dest_acc_en = false;
            bool math_approx_mode = false;
            auto mm_kernel = tt_metal::CreateComputeKernel(
                program,
                "kernels/compute/matmul_large_block_zm.cpp",
                core,
                mm_args,
                MathFidelity::HiFi4,
                fp32_dest_acc_en,
                math_approx_mode
            );

            // Write runtime args to device
            std::vector<uint32_t> mm_reader_args = {
                (std::uint32_t) in0_dram_addr, // in0_tensor_addr
                (std::uint32_t)  K * per_core_M * output_idx_y, // in0_tensor_start_tile_id
                (std::uint32_t)  1, // in0_tensor_stride_w
                (std::uint32_t)  K, // in0_tensor_stride_h
                (std::uint32_t)  in0_block_w, // in0_tensor_next_block_stride

                (std::uint32_t)  in0_block_w, // in0_block_w
                (std::uint32_t)  per_core_M, // in0_block_h
                (std::uint32_t)  in0_block_w * per_core_M, //in0_block_num_tiles

                (std::uint32_t)  in1_dram_addr, // in1_tensor_addr
                (std::uint32_t)  per_core_N * output_idx_x, //in1_tensor_start_tile_id
                (std::uint32_t)  1, // in1_tensor_stride_w
                (std::uint32_t)  N, // in1_tensor_stride_h
                (std::uint32_t)  in0_block_w * N, //in1_tensor_next_block_stride

                (std::uint32_t)  per_core_N, // in1_block_w
                (std::uint32_t)  in0_block_w, //in1_block_h
                (std::uint32_t)  per_core_N * in0_block_w, // in1_block_num_tiles

                (std::uint32_t)  K / in0_block_w // num_blocks
            };

            std::vector<uint32_t> writer_args = {
                (std::uint32_t) out_dram_addr, // out_tensor_addr
                (std::uint32_t) output_idx_x * per_core_N + output_idx_y * per_core_M * N, // out_tensor_start_tile_id
                (std::uint32_t) 1, // out_tensor_stride_w
                (std::uint32_t) N,  // out_tensor_stride_h
                (std::uint32_t) out_subblock_w, // out_tensor_next_subblock_stride_w
                (std::uint32_t) out_subblock_h * N, // out_tensor_next_subblock_stride_h

                (std::uint32_t) out_subblock_w, // out_subblock_w
                (std::uint32_t) out_subblock_h, // out_subblock_h
                (std::uint32_t) (out_subblock_w * out_subblock_h), // out_subblocks_w * out_subblocks_h
                (std::uint32_t) (per_core_N / out_subblock_w), // out_num_subblocks_w
                (std::uint32_t) (per_core_M / out_subblock_h), // out_num_subblocks_h
            };

            tt_metal::WriteRuntimeArgsToDevice(device, mm_reader_kernel, core, mm_reader_args);
            tt_metal::WriteRuntimeArgsToDevice(device, unary_writer_kernel, core, writer_args);

            num_blocks_read++;
        }
    }

    return program;
}


namespace tt {

namespace tt_metal {


Tensor matmul_multi_core_reuse_(const Tensor &a, const Tensor &b, bool bcast_batch) {

    const auto& ashape = a.shape(), bshape = b.shape();

    // TODO: Build some sort of dispatcher based on location of op operands
    TT_ASSERT(not a.on_host() and not b.on_host(), "Operands to matmul need to be on device!");
    TT_ASSERT(a.device() == b.device(), "Operands to matmul need to be on the same device!");
    TT_ASSERT(a.buffer() != nullptr and b.buffer() != nullptr, "Operands to matmul need to be allocated in buffers on device!");

    uint32_t single_tile_size = 2 * 1024;
    tt_metal::Buffer *src0_dram_buffer = a.buffer();
    tt_metal::Buffer *src1_dram_buffer = b.buffer();
    if (bcast_batch)
        TT_ASSERT(bshape[0]*bshape[1] == 1 && "matmul (batch bcast variant) expects input tensors of shapes BCMK*11KN=BCMN");
    else {
        // same condition as above, different message
        TT_ASSERT(ashape[1] == bshape[1] && ashape[0] == bshape[0]
            && "bmm (non-bcast matmul) expects input tensors of shapes BCMK*BCKN=BCMN");
    }
    TT_ASSERT(src0_dram_buffer->size() % single_tile_size == 0);
    TT_ASSERT(src1_dram_buffer->size() % single_tile_size == 0);

    TT_ASSERT(ashape[0] * ashape[1] == 1, "Batch dimensions must be 1 for fast matmul"); // TODO: Support batch
    TT_ASSERT(bshape[0] * bshape[1] == 1, "Batch dimensions must be 1 for fast matmul");
    TT_ASSERT(ashape[3] == bshape[2], "Dimension K (A.shape[2] and B.shape[3]) must match for A and B in bmm_op"); // A.K == B.K
    TT_ASSERT(ashape[2] % TILE_HEIGHT == 0);
    TT_ASSERT(ashape[3] % TILE_WIDTH == 0);
    TT_ASSERT(bshape[2] % TILE_HEIGHT == 0);
    TT_ASSERT(bshape[3] % TILE_WIDTH == 0);

    ////////////////////////////////////////////////////////////////////////////
    //                      Matmul Parameters Setup
    ////////////////////////////////////////////////////////////////////////////
    // NOTE: Only supports matmuls where output is blocks of 16 x 16 tiles (ie. multiples of 16*32 x 16*32)
    // NOTE: Maximum number of tiles in output is 120 * 16^2 = 30,720 (eg. [1, 1, 5120, 6144])
    // uint32_t B = ashape[0]*ashape[1]; // Only supports B = 1?
    uint32_t Mt = ashape[2]/TILE_HEIGHT;
    uint32_t Kt = ashape[3]/TILE_WIDTH;
    uint32_t Nt = bshape[3]/TILE_WIDTH;
    uint32_t in0_block_w = 2;
    uint32_t out_subblock_h = 4;
    uint32_t out_subblock_w = 2;
    uint32_t per_core_M = 16;
    uint32_t per_core_N = 16;

    TT_ASSERT(Mt % per_core_M == 0);
    TT_ASSERT(Nt % per_core_N == 0);
    TT_ASSERT(Kt % in0_block_w == 0);

    // This should allocate a DRAM buffer on the device
    tt_metal::Device *device = a.device();
    auto logical_grid_size = device->logical_grid_size();
    uint32_t num_cores_x = logical_grid_size.x;
    uint32_t num_cores_y = logical_grid_size.y;

    uint32_t num_blocks_total = (Mt / per_core_M) * (Nt / per_core_N);
    TT_ASSERT(num_blocks_total <= num_cores_x * num_cores_y);

    ////////////////////////////////////////////////////////////////////////////
    //                      Grayskull Device Setup
    ////////////////////////////////////////////////////////////////////////////
    std::array<uint32_t, 4> cshape{ashape[0], ashape[1], ashape[2], bshape[3]}; // C=A*B, N1MK*11KN->N1MN
    tt_metal::Tensor output = tt_metal::Tensor(cshape, a.dtype(), tt::tt_metal::Layout::TILE, device);
    tt_metal::Buffer *dst_dram_buffer = output.buffer();
    TT_ASSERT(dst_dram_buffer != nullptr, "Output buffer should be allocated on device!");

    uint32_t in0_dram_addr = src0_dram_buffer->address();
    uint32_t in1_dram_addr = src1_dram_buffer->address();
    uint32_t out_dram_addr = dst_dram_buffer->address();

    ////////////////////////////////////////////////////////////////////////////
    //                      Application Setup
    ////////////////////////////////////////////////////////////////////////////
    uint32_t dram_buffer_size_act = src0_dram_buffer->size(); // num_tiles of FP16_B, hard-coded in the reader/writer kernels
    uint32_t dram_buffer_size_weights = src1_dram_buffer->size(); // num_tiles of FP16_B, hard-coded in the reader/writer kernels
    uint32_t dram_buffer_size_out = dst_dram_buffer->size(); // num_tiles of FP16_B, hard-coded in the reader/writer kernels

    TT_ASSERT(in0_dram_addr + dram_buffer_size_act < 1024 * 1024 * 1024);
    TT_ASSERT(in1_dram_addr + dram_buffer_size_weights < 1024 * 1024 * 1024);
    TT_ASSERT(out_dram_addr + dram_buffer_size_out < 1024 * 1024 * 1024);

    tt_metal::Program * program = create_program(
        device,
        single_tile_size,
        num_cores_x,
        Mt, Nt, Kt,
        in0_block_w,
        out_subblock_h, out_subblock_w,
        per_core_M, per_core_N,
        in0_dram_addr, in1_dram_addr, out_dram_addr
    );

    ////////////////////////////////////////////////////////////////////////////
    //                      Compile Application
    ////////////////////////////////////////////////////////////////////////////
    bool pass = true;
    constexpr bool skip_hlkc = false;
    pass &= tt_metal::CompileProgram(device, program, skip_hlkc);

    ////////////////////////////////////////////////////////////////////////////
    //                      Execute Application
    ////////////////////////////////////////////////////////////////////////////
    pass &= tt_metal::ConfigureDeviceWithProgram(device, program);
    pass &= tt_metal::LaunchKernels(device, program);

    delete program;

    TT_ASSERT(pass);

    // output does not hold any data, contains pointer to buffer on device with the data
    return output;
}

Tensor matmul_multi_core_reuse(const Tensor& a, const Tensor& b) {
    return matmul_multi_core_reuse_(a, b, true);
}

Tensor bmm_multi_core_reuse(const Tensor& a, const Tensor& b) {
    return matmul_multi_core_reuse_(a, b, false);
}

}  // namespace tt_metal

}  // namespace tt
