# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from argparse import ArgumentParser
from pathlib import Path

from .parsers import keys_and_sizes

""" ----------------------------------------------------------------------- """


def _add_manager_arguments(parser: ArgumentParser) -> None:
    # Pools and leases
    parser.add_argument(
        "--pools", type=keys_and_sizes, default=keys_and_sizes("1:20GiB")
    )
    parser.add_argument("--lease-ttl-ms", type=int, default=10000)

    # Eviction
    parser.add_argument("--min-water-level", type=int, default=80)
    parser.add_argument("--max-water-level", type=int, default=90)
    parser.add_argument(
        "--eviction-policy",
        choices=("lru", "sharded_lru"),
        default="sharded_lru",
    )
    parser.add_argument("--lru-shards", type=int, default=16)


def _add_simulator_arguments(parser: ArgumentParser) -> None:
    # Metrics and output
    parser.add_argument("--detailed-stats", action="store_true")
    parser.add_argument("--memory-plot", type=Path)
    parser.add_argument("--profile", action="store_true")

    # Sampling intervals
    parser.add_argument(
        "--latency-sample-interval",
        type=int,
        default=0,
        help="0 means final only",
    )
    parser.add_argument(
        "--memory-sample-interval",
        type=int,
        default=0,
        help="0 means final only",
    )

    # Utility
    parser.add_argument("--progress-bar", action="store_true")


""" ----------------------------------------------------------------------- """


def add_common_arguments(parser: ArgumentParser) -> None:
    _add_manager_arguments(parser)
    _add_simulator_arguments(parser)
