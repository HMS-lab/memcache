# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import argparse

from tqdm import tqdm

from meta_manager_sim import (
    RandomWorkload,
    RunConfig,
    add_common_arguments,
    run_simulation,
    sizes_and_values,
)


def main() -> None:
    parser = argparse.ArgumentParser()
    add_common_arguments(parser)

    # Add random-workload-specific CLI arguments
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--num-reads", type=int, required=True)
    parser.add_argument("--num-blocks", type=int, required=True)
    parser.add_argument("--size-weights", type=sizes_and_values, required=True)
    parser.add_argument("--hot-block-fraction", type=float, default=0.2)
    parser.add_argument("--hot-read-probability", type=float, default=0.8)

    args = parser.parse_args()
    config = RunConfig.from_args(args)

    # Prepare a stream of random block reads
    reads = RandomWorkload(args.seed).reads(
        args.num_reads,
        args.num_blocks,
        args.size_weights,
        args.hot_block_fraction,
        args.hot_read_probability,
    )

    if config.progress_bar:
        reads = tqdm(reads, total=args.num_reads)

    run_simulation(reads, config)


if __name__ == "__main__":
    main()
