// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
#pragma once

#include "ttnn/run_operation.hpp"
#include "ttnn/tensor/tensor.hpp"

namespace ttnn::operations::experimental::ssm {

struct RepeatAndInterleaveEltwiseMul {
    MemoryConfig memory_config;
    DataType dtype;
    MathFidelity math_fidelity;

    const uint32_t HIDDEN_SIZE = 5120;

    void validate(const std::vector<Tensor>& input_tensors) const;
    std::vector<tt::tt_metal::LegacyShape> compute_output_shapes(const std::vector<Tensor>& input_tensors) const;
    std::vector<Tensor> create_output_tensors(const std::vector<Tensor>& input_tensors) const;
    operation::ProgramWithCallbacks create_program(
        const std::vector<Tensor>& input_tensors, std::vector<Tensor>& output_tensors) const;
};

}  // namespace ttnn::operations::experimental::ssm
