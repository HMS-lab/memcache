# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Run seeded shared-key access sources."""

import argparse

from meta_manager_sim.simulation import run_simulation
from meta_manager_sim.utils.cli import (
    add_common_arguments,
    duration,
    manager_configuration,
    report_output,
    run_configuration,
)
from meta_manager_sim.utils.sizes import byte_size
from meta_manager_sim.workloads.random.random_workload import (
    RandomConfig,
    prepare_random,
)


def weighted_size(text: str) -> tuple[int, float]:
    size, weight = text.rsplit(":", 1)
    return byte_size(size), float(weight)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--accesses", type=int, default=10000)
    result.add_argument("--key-count", type=int, default=10000)
    result.add_argument("--size-weight", type=weighted_size, action="append")
    result.add_argument("--sources", type=int, default=1)
    result.add_argument("--access-interval", type=duration, default=1000000)
    result.add_argument("--hot-key-fraction", type=float, default=0.2)
    result.add_argument("--hot-read-probability", type=float, default=0.8)
    result.add_argument("--seed", type=int, default=42)
    add_common_arguments(result)
    return result


def main(argv: list[str] | None = None) -> int:
    arguments = parser()
    args = arguments.parse_args(argv)
    try:
        config = RandomConfig(
            accesses=args.accesses,
            key_count=args.key_count,
            size_weights=tuple(
                args.size_weight or RandomConfig().size_weights
            ),
            source_count=args.sources,
            seed=args.seed,
            access_interval_ns=args.access_interval,
            hot_key_fraction=args.hot_key_fraction,
            hot_read_probability=args.hot_read_probability,
        )
        report = run_simulation(
            prepare_random(config),
            manager_configuration(args),
            run_configuration(args),
        )
        report_output(report, args)
    except (ValueError, RuntimeError, OSError) as error:
        arguments.exit(2, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
