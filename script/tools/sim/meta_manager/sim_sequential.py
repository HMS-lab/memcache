# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import argparse

from tqdm import tqdm

from meta_manager_sim import (
    RunConfig,
    SequentialWorkload,
    add_common_arguments,
    run_simulation,
    size_bytes,
)


def main() -> None:
    parser = argparse.ArgumentParser()
    add_common_arguments(parser)

    # Add sequential-workload-specific CLI arguments
    parser.add_argument("--num-reads", type=int, required=True)
    parser.add_argument("--block-size", type=size_bytes, required=True)

    args = parser.parse_args()
    config = RunConfig.from_args(args)

    # Prepare a stream of sequential block reads
    reads = SequentialWorkload().reads(args.num_reads, args.block_size)

    if config.progress_bar:
        reads = tqdm(reads, total=args.num_reads)

    run_simulation(reads, config)


if __name__ == "__main__":
    main()
