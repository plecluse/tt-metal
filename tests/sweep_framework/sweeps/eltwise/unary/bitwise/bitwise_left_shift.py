# SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.

# SPDX-License-Identifier: Apache-2.0

from typing import Optional, Tuple
from functools import partial

import torch
import random
import ttnn
from tests.sweep_framework.sweep_utils.utils import gen_shapes, gen_rand_bitwise_left_shift
from tests.tt_eager.python_api_testing.sweep_tests.generation_funcs import gen_func_with_cast_tt

from tests.ttnn.utils_for_testing import check_with_pcc, start_measuring_time, stop_measuring_time
from models.utility_functions import torch_random

# Override the default timeout in seconds for hang detection.
TIMEOUT = 30

random.seed(0)

# Parameters provided to the test vector generator are defined here.
# They are defined as dict-type suites that contain the arguments to the run function as keys, and lists of possible inputs as values.
# Each suite has a key name (in this case "suite_1") which will associate the test vectors to this specific suite of inputs.
# Developers can create their own generator functions and pass them to the parameters as inputs.
parameters = {
    "nightly": {
        "input_shape": gen_shapes([1, 1, 32, 32], [6, 12, 128, 128], [1, 1, 32, 32], 4)
        + gen_shapes([1, 32, 32], [12, 256, 256], [1, 32, 32], 4)
        + gen_shapes([32, 32], [256, 256], [32, 32], 4),
        "shift_bits": list(range(1, 31)),
        "use_safe_nums": [True],
        "input_a_dtype": [ttnn.int32],
        "input_a_layout": [ttnn.TILE_LAYOUT],
        "input_a_memory_config": [ttnn.DRAM_MEMORY_CONFIG, ttnn.L1_MEMORY_CONFIG],
        "output_memory_config": [ttnn.DRAM_MEMORY_CONFIG, ttnn.L1_MEMORY_CONFIG],
    },
    "xfail": {
        "input_shape": gen_shapes([1, 1, 32, 32], [6, 12, 128, 128], [1, 1, 32, 32], 4)
        + gen_shapes([1, 32, 32], [12, 256, 256], [1, 32, 32], 4)
        + gen_shapes([32, 32], [256, 256], [32, 32], 4),
        "shift_bits": list(range(1, 31)),
        "use_safe_nums": [False],
        "input_a_dtype": [ttnn.int32],
        "input_a_layout": [ttnn.TILE_LAYOUT],
        "input_a_memory_config": [ttnn.DRAM_MEMORY_CONFIG, ttnn.L1_MEMORY_CONFIG],
        "output_memory_config": [ttnn.DRAM_MEMORY_CONFIG, ttnn.L1_MEMORY_CONFIG],
    },
}


def mesh_device_fixture():
    device = ttnn.open_device(device_id=0)
    assert ttnn.device.is_wormhole_b0(device), "This op is available for Wormhole_B0 only"
    yield (device, "Wormhole_B0")
    ttnn.close_device(device)
    del device


# This is the run instructions for the test, defined by the developer.
# The run function must take the above-defined parameters as inputs.
# The runner will call this run function with each test vector, and the returned results from this function will be stored.
# If you defined a device_mesh_fixture above, the object you yielded will be passed into this function as 'device'. Otherwise, it will be the default ttnn device opened by the infra.
def run(
    input_shape,
    shift_bits,
    use_safe_nums,
    input_a_dtype,
    input_a_layout,
    input_a_memory_config,
    output_memory_config,
    *,
    device,
) -> list:
    data_seed = random.randint(0, 20000000)
    torch.manual_seed(data_seed)

    # In ttnn.bitwise_left_shift, only bits from positions 0 to 30 are included during shifting (sign bit remains the same).
    # That is not the case with torch.bitwise_left_shift, all of the bits are included during shifting.
    # use_safe_nums argument makes sure that those two bits are the same, so the results between the two versions are the same.
    if use_safe_nums is True:
        torch_input_tensor_a = gen_func_with_cast_tt(
            partial(gen_rand_bitwise_left_shift, shift_bits=shift_bits, low=-2147483647, high=2147483648), input_a_dtype
        )(input_shape)
    else:
        torch_input_tensor_a = gen_func_with_cast_tt(
            partial(torch_random, low=-2147483647, high=2147483648, dtype=torch.int64), input_a_dtype
        )(input_shape)

    torch_output_tensor = torch.bitwise_left_shift(torch_input_tensor_a, shift_bits).to(torch.int32)

    input_tensor_a = ttnn.from_torch(
        torch_input_tensor_a,
        dtype=input_a_dtype,
        layout=input_a_layout,
        device=device,
        memory_config=input_a_memory_config,
    )

    start_time = start_measuring_time()
    result = ttnn.bitwise_left_shift(input_tensor_a, shift_bits=shift_bits, memory_config=output_memory_config)
    output_tensor = ttnn.to_torch(result).to(torch.int32)
    e2e_perf = stop_measuring_time(start_time)

    return [check_with_pcc(torch_output_tensor, output_tensor, 0.999), e2e_perf]
