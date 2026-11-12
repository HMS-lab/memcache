# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Shared parser settings and output; no simulation logic."""

import argparse
import re
from pathlib import Path

from meta_manager_sim.config import ManagerConfig, RunConfig
from meta_manager_sim.profiling.models import SimulationResult
from meta_manager_sim.profiling.reporting import print_report, write_reports
from meta_manager_sim.utils.sizes import byte_size


def duration(text: str) -> int:
    match = re.fullmatch(r"(\d+)(ns|us|ms|s)", text)
    if match is None:
        raise ValueError("Use an integer duration with ns/us/ms/s")
    return (
        int(match[1])
        * {"ns": 1, "us": 1000, "ms": 1000000, "s": 1000000000}[match[2]]
    )


def add_common_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--capacity", type=byte_size, default=64 << 20)
    parser.add_argument("--high-watermark", type=int, default=90)
    parser.add_argument("--low-watermark", type=int, default=80)
    parser.add_argument(
        "--eviction-policy", choices=("lru", "sharded_lru"), default="lru"
    )
    parser.add_argument("--lru-shards", type=int, default=16)
    parser.add_argument("--lease-ttl-ms", type=int, default=1000)
    parser.add_argument("--cleanup-timeout-ms", type=int, default=5000)
    parser.add_argument("--max-value-alignment", action="store_true")
    parser.add_argument(
        "--execution", choices=("ordered", "concurrent"), default="ordered"
    )
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--read-hold", type=duration, default=0)
    parser.add_argument("--write-hold", type=duration, default=0)
    parser.add_argument("--sample-every", type=int, default=0)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--csv-dir", dest="csv", type=Path)


def manager_configuration(args: argparse.Namespace) -> ManagerConfig:
    return ManagerConfig(
        args.capacity,
        args.low_watermark,
        args.high_watermark,
        args.eviction_policy,
        args.lru_shards,
        args.lease_ttl_ms,
        args.cleanup_timeout_ms,
        args.max_value_alignment,
    )


def run_configuration(args: argparse.Namespace) -> RunConfig:
    return RunConfig(
        args.execution,
        args.workers,
        args.read_hold,
        args.write_hold,
        args.sample_every,
    )


def report_output(report: SimulationResult, args: argparse.Namespace) -> None:
    write_reports(report, args.json, args.csv)
    print_report(report)
