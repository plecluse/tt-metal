// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ttnn/tensor/tensor.hpp"
#include "ttnn/run_operation.hpp"

namespace ttnn::operations::data_movement {

struct SplitDeviceOperation {
    const int num_splits;
    const int dim;
    const MemoryConfig output_mem_config;

    void validate(const std::vector<Tensor>& input_tensors) const;
    std::vector<tt::tt_metal::LegacyShape> compute_output_shapes(const std::vector<Tensor>& input_tensors) const;
    std::vector<Tensor> create_output_tensors(const std::vector<Tensor>& input_tensors) const;
    operation::ProgramWithCallbacks create_program(
        const std::vector<Tensor>& input_tensors, std::vector<Tensor>& output_tensors) const;
};

}  // namespace ttnn::operations::data_movement
