# SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.

# SPDX-License-Identifier: Apache-2.0

import pytest
from loguru import logger
import ttnn
from models.utility_functions import is_wormhole_b0, is_grayskull, skip_for_wormhole_b0
from models.utility_functions import torch2tt_tensor, tt2torch_tensor, pad_by_zero, roundup32
import torch
from tests.tt_eager.python_api_testing.sweep_tests.comparison_funcs import (
    comp_equal,
    comp_pcc,
)
import random
import math
from models.utility_functions import is_wormhole_b0, is_grayskull, is_wormhole_b0, is_blackhole


random.seed(10)


def num_cores_to_rectangle_grid(num_cores, device):
    """
    Find a rectangular core grid size, given an number of cores.

    Return None if rectangle grid is not possible.
    """
    x = device.compute_with_storage_grid_size().x
    while x > 0 and num_cores % x != 0:
        x -= 1

    if x == 0:
        return None

    y = num_cores // x
    return (x, y)


def get_physical_to_logical_core_mapping(device):
    """
    Get a mapping from physical core coords to logical core coords

    Returns a dictionary.
    """
    mapping = {}
    grid = device.compute_with_storage_grid_size()
    for x in range(grid.x):
        for y in range(grid.y):
            physical_core = device.worker_core_from_logical_core(ttnn.CoreCoord(x, y))
            mapping[(physical_core.x, physical_core.y)] = (x, y)
    return mapping


PREFETCHER_GRID = [
    (8, 11),
    (8, 9),
    (8, 8),
    (8, 7),
    (8, 5),
    (8, 3),
    (8, 2),
    (8, 1),
    (7, 1),
    (7, 2),
    (7, 3),
    (7, 5),
    (7, 7),
    (7, 8),
    (7, 9),
    (7, 11),
    (3, 11),
    (3, 7),
    (3, 5),
    (3, 1),
    (2, 1),
    (2, 5),
    (2, 7),
    (2, 11),
]


def run_multi_core_matmul_1d(
    device,
    in0_dtype,
    in1_dtype,
    fidelity,
    has_bias,
    fp32_acc_mode,
    packer_l1_acc,
    B,
    M,
    K,
    N,
    activation,
    grid,
    use_arbitrary_cores,
    num_iters,
    max_dst_tiles=8,
    pcc_threshold=0.98,
):
    assert not has_bias, "Bias not supported for gather_in0 mode."
    if not isinstance(grid, tuple) and not use_arbitrary_cores:
        pytest.skip("Grid is not a tuple and not using arbitrary cores")

    in0_shape = [1, B, M, K]
    in1_shape = [1, 1, K, N]
    num_cores = grid[0] * grid[1] if isinstance(grid, tuple) else len(grid)

    storage_grid = num_cores_to_rectangle_grid(num_cores, device)
    if storage_grid is None:
        pytest.skip(f"Could not find a rectangle grid for num_cores: {num_cores}")

    M *= B  # Fuse batch always enabled

    in0_block_h = M // ttnn.TILE_SIZE
    in0_block_w = K // num_cores // ttnn.TILE_SIZE
    out_block_h = M // ttnn.TILE_SIZE
    out_block_w = N // num_cores // ttnn.TILE_SIZE

    num_blocks_y = (M // ttnn.TILE_SIZE - 1) // out_block_h + 1
    num_blocks_x = (N // ttnn.TILE_SIZE - 1) // out_block_w + 1
    num_blocks_total = num_blocks_y * num_blocks_x

    if num_blocks_total != num_cores:
        pytest.skip(f"num_blocks_total {num_blocks_total} != num_cores {num_cores}")

    out_subblock_h = 1
    out_subblock_w = max_dst_tiles if (out_block_h == 1 and out_block_w <= max_dst_tiles) else 4
    while out_block_w % out_subblock_w != 0:
        out_subblock_w -= 1

    logger.debug("in0 block h w " + str(in0_block_h) + " " + str(in0_block_w))
    logger.debug("in1 block h w " + str(in0_block_w) + " " + str(out_block_w))
    logger.debug("out block h w " + str(out_block_h) + " " + str(out_block_w))
    logger.debug("out subblock h w " + str(out_subblock_h) + " " + str(out_subblock_w))

    if use_arbitrary_cores:
        # x, y
        if isinstance(grid, tuple):  # Generate random grid
            CORE_RANGE = [(x, y) for y in range(storage_grid[1]) for x in range(storage_grid[0])]
            random.shuffle(CORE_RANGE)
        else:  # Use custom grid
            mapping = get_physical_to_logical_core_mapping(device)
            CORE_RANGE = [mapping[physical_coord] for physical_coord in grid]

        core_range_set = ttnn.CoreRangeSet(
            [
                ttnn.CoreRange(
                    ttnn.CoreCoord(x, y),
                    ttnn.CoreCoord(x, y),
                )
                for x, y in CORE_RANGE
            ]
        )
    else:
        core_range_set = ttnn.CoreRangeSet(
            {
                ttnn.CoreRange(
                    ttnn.CoreCoord(0, 0),
                    ttnn.CoreCoord(storage_grid[0] - 1, storage_grid[1] - 1),
                ),
            }
        )

    in0_sharded_mem_config = ttnn.MemoryConfig(
        ttnn.TensorMemoryLayout.WIDTH_SHARDED,
        ttnn.BufferType.L1,
        ttnn.ShardSpec(
            core_range_set,
            [M, K // num_cores],
            ttnn.ShardOrientation.ROW_MAJOR,
            False,
        ),
    )

    in1_sharded_mem_config = ttnn.MemoryConfig(
        ttnn.TensorMemoryLayout.WIDTH_SHARDED,
        ttnn.BufferType.L1,
        ttnn.ShardSpec(
            core_range_set,
            [K, N // num_cores],
            ttnn.ShardOrientation.ROW_MAJOR,
            False,
        ),
    )

    output_sharded_mem_config = ttnn.MemoryConfig(
        ttnn.TensorMemoryLayout.WIDTH_SHARDED,
        ttnn.BufferType.L1,
        ttnn.ShardSpec(
            core_range_set,
            [M, N // num_cores],
            ttnn.ShardOrientation.ROW_MAJOR,
            False,
        ),
    )

    in0 = torch.randn(in0_shape)
    in1 = torch.randn(in1_shape)

    in0_t = ttnn.from_torch(
        in0,
        device=device,
        layout=ttnn.TILE_LAYOUT,
        dtype=in0_dtype,
        memory_config=in0_sharded_mem_config,
    )
    in1_t = ttnn.from_torch(
        in1,
        device=device,
        layout=ttnn.TILE_LAYOUT,
        dtype=in1_dtype,
        memory_config=in1_sharded_mem_config,
    )

    program_config = ttnn.MatmulMultiCoreReuseMultiCast1DProgramConfig(
        compute_with_storage_grid_size=storage_grid,
        in0_block_w=in0_block_w,
        out_subblock_h=out_subblock_h,
        out_subblock_w=out_subblock_w,
        per_core_M=out_block_h,
        per_core_N=out_block_w,
        fuse_batch=True,
        fused_activation=activation,
        mcast_in0=False,
        gather_in0=True,
    )

    compute_kernel_config = ttnn.WormholeComputeKernelConfig(
        math_fidelity=fidelity,
        math_approx_mode=True,
        fp32_dest_acc_en=fp32_acc_mode,
        packer_l1_acc=packer_l1_acc,
        dst_full_sync_en=True,
    )

    for _ in range(num_iters):
        output_t = ttnn.matmul(
            in0_t,
            in1_t,
            program_config=program_config,
            memory_config=output_sharded_mem_config,
            compute_kernel_config=compute_kernel_config,
        )
    tt_out = ttnn.to_torch(output_t)

    pt_out = in0 @ in1
    if activation:
        act_fnc = torch.nn.functional.silu if activation == ttnn.UnaryOpType.SILU else torch.nn.functional.relu
        pt_out = act_fnc(pt_out)

    passing, output = comp_pcc(pt_out, tt_out, pcc_threshold)
    logger.info(output)

    assert passing

    # Check program cache
    assert device.num_program_cache_entries() == 1  # Only 1 op


@pytest.mark.skipif(is_grayskull(), reason="GS does not support fp32")
@pytest.mark.skipif(is_blackhole(), reason="Test suite for GS only")
@pytest.mark.parametrize("has_bias", [False], ids=["no_bias"])
@pytest.mark.parametrize(
    "B, M, K, N, in0_dtype, in1_dtype, fidelity, packer_l1_acc, fp32_acc_mode, grid",
    [
        # # 32, 2304, 3840 (PREFETCHER), only works on TG
        # (1, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat4_b, ttnn.MathFidelity.LoFi, True, True, PREFETCHER_GRID),
        # 32, 2304, 3840
        (1, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat4_b, ttnn.MathFidelity.LoFi, True, True, (8, 3)),
        # 32, 2304, 3840
        (3, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat4_b, ttnn.MathFidelity.LoFi, True, True, (8, 3)),
        # 32, 2304, 3840
        (3, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.LoFi, False, False, (8, 3)),
        # 32, 2304, 3840
        (3, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi2, False, True, (8, 3)),
        # 32, 2304, 3840
        (3, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi2, True, False, (8, 3)),
        # 32, 2304, 3840
        (3, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi4, False, False, (8, 3)),
        # 32, 2304, 3840
        (3, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi4, True, False, (8, 3)),
        # 32, 2304, 3840
        (3, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi4, False, True, (8, 3)),
        # 256, 1024, 8192
        (1, 256, 1024, 8192, ttnn.bfloat16, ttnn.bfloat4_b, ttnn.MathFidelity.HiFi4, True, True, (8, 4)),
        # 256, 1024, 8192
        (1, 256, 1024, 8192, ttnn.bfloat16, ttnn.bfloat4_b, ttnn.MathFidelity.HiFi4, True, True, (8, 4)),
        # # 128, 8192, 2048
        # (1, 128, 8192, 2048, ttnn.bfloat8_b, ttnn.bfloat4_b, ttnn.MathFidelity.HiFi2, True, True, (8, 8)),
        # # 128, 8192, 2048
        # (1, 128, 8192, 2048, ttnn.bfloat8_b, ttnn.bfloat4_b, ttnn.MathFidelity.HiFi2, True, False, (8, 8)),
        # # 128, 8192, 2048
        # (1, 128, 8192, 2048, ttnn.bfloat8_b, ttnn.bfloat4_b, ttnn.MathFidelity.HiFi2, False, True, (8, 8)), # Fails with 0.98 PCC
        # 32, 64, 64
        (1, 32, 64, 64, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi4, True, True, (2, 1)),
        # 32, 64, 64
        (11, 32, 64, 64, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi4, True, True, (2, 1)),
    ],
)
@pytest.mark.parametrize(
    "activation",
    [
        None,
        ttnn.UnaryOpType.SILU,
        ttnn.UnaryOpType.RELU,
    ],
)
@pytest.mark.parametrize(
    "use_arbitrary_cores",
    [False, True],
)
@pytest.mark.parametrize(
    "num_iters",
    [1, 3],
)
def test_multi_core_matmul_1d_wh(
    device,
    in0_dtype,
    in1_dtype,
    fidelity,
    has_bias,
    fp32_acc_mode,
    packer_l1_acc,
    B,
    M,
    K,
    N,
    activation,
    grid,
    use_arbitrary_cores,
    num_iters,
    use_program_cache,
    function_level_defaults,
):
    run_multi_core_matmul_1d(
        device,
        in0_dtype,
        in1_dtype,
        fidelity,
        has_bias,
        fp32_acc_mode,
        packer_l1_acc,
        B,
        M,
        K,
        N,
        activation,
        grid,
        use_arbitrary_cores,
        num_iters,
    )


@pytest.mark.skipif(is_wormhole_b0(), reason="Test suite for GS only")
@pytest.mark.skipif(is_blackhole(), reason="Test suite for GS only")
@pytest.mark.parametrize("has_bias", [False], ids=["no_bias"])
@pytest.mark.parametrize(
    "B, M, K, N, in0_dtype, in1_dtype, fidelity, packer_l1_acc, fp32_acc_mode, grid",
    [
        # 32, 2304, 3840
        (1, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat4_b, ttnn.MathFidelity.LoFi, False, False, (8, 3)),
        # 32, 2304, 3840
        (3, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.LoFi, False, False, (8, 3)),
        # 32, 2304, 3840
        (3, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi2, False, False, (8, 3)),
        # 32, 2304, 3840
        (3, 32, 2304, 3840, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi4, False, False, (8, 3)),
        # 256, 1024, 8192
        (1, 256, 1024, 8192, ttnn.bfloat16, ttnn.bfloat4_b, ttnn.MathFidelity.HiFi4, False, False, (8, 4)),
        # 128, 8192, 2048
        (1, 128, 4096, 2048, ttnn.bfloat8_b, ttnn.bfloat4_b, ttnn.MathFidelity.HiFi2, False, False, (8, 8)),
        # 32, 64, 64
        (1, 32, 64, 64, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi4, False, False, (2, 1)),
        # 32, 64, 64
        (11, 32, 64, 64, ttnn.bfloat16, ttnn.bfloat8_b, ttnn.MathFidelity.HiFi4, False, False, (2, 1)),
    ],
)
@pytest.mark.parametrize(
    "activation",
    [
        None,
        ttnn.UnaryOpType.SILU,
        ttnn.UnaryOpType.RELU,
    ],
)
@pytest.mark.parametrize(
    "use_arbitrary_cores",
    [False, True],
)
@pytest.mark.parametrize(
    "num_iters",
    [1, 3],
)
def test_multi_core_matmul_1d_gs(
    device,
    in0_dtype,
    in1_dtype,
    fidelity,
    has_bias,
    fp32_acc_mode,
    packer_l1_acc,
    B,
    M,
    K,
    N,
    activation,
    grid,
    use_arbitrary_cores,
    num_iters,
    use_program_cache,
    function_level_defaults,
):
    run_multi_core_matmul_1d(
        device,
        in0_dtype,
        in1_dtype,
        fidelity,
        has_bias,
        fp32_acc_mode,
        packer_l1_acc,
        B,
        M,
        K,
        N,
        activation,
        grid,
        use_arbitrary_cores,
        num_iters,
        pcc_threshold=0.96,
    )
