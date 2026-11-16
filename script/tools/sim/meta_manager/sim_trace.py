# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import argparse
from pathlib import Path

from tqdm import tqdm

from meta_manager_sim import (
    RunConfig,
    TraceWorkload,
    add_common_arguments,
    key_and_size,
    run_simulation,
)


def main() -> None:
    parser = argparse.ArgumentParser()
    add_common_arguments(parser)

    # Add trace-workload-specific CLI arguments
    parser.add_argument("--trace-path", type=Path, required=True)
    parser.add_argument("--line", type=int, default=0)
    parser.add_argument(
        "--model-block-size",
        type=key_and_size,
        action="append",
        dest="model_block_sizes",
        required=True,
    )

    args = parser.parse_args()
    config = RunConfig.from_args(args)

    # Prepare a stream of trace-sourced block reads
    reads = TraceWorkload(args.trace_path, args.line).reads(
        dict(args.model_block_sizes)
    )

    if config.progress_bar:
        # Note: The number of reads is not predetermined
        reads = tqdm(list(reads))

    run_simulation(reads, config)


if __name__ == "__main__":
    main()
