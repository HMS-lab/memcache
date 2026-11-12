# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Replay shared model:hash block accesses."""

import argparse
from pathlib import Path

from meta_manager_sim.simulation import run_simulation
from meta_manager_sim.utils.cli import (
    add_common_arguments,
    manager_configuration,
    report_output,
    run_configuration,
)
from meta_manager_sim.utils.sizes import byte_size
from meta_manager_sim.workloads.trace.trace_reader import (
    LineSelection,
    parse_line_selection,
)
from meta_manager_sim.workloads.trace.trace_workload import (
    TraceConfig,
    prepare_trace,
)


def model_size(text: str) -> tuple[str, int]:
    model, size = text.rsplit(":", 1)
    return model, byte_size(size)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--trace-path", required=True, type=Path)
    result.add_argument(
        "--lines", type=parse_line_selection, default=LineSelection()
    )
    result.add_argument(
        "--model-block-size", type=model_size, action="append", required=True
    )
    result.add_argument(
        "--timestamp-unit", choices=("s", "ms", "us", "ns"), default="s"
    )
    add_common_arguments(result)
    return result


def main(argv: list[str] | None = None) -> int:
    arguments = parser()
    args = arguments.parse_args(argv)
    try:
        if args.json and args.json.resolve() == args.trace_path.resolve():
            raise ValueError("Report must not overwrite trace")
        if args.csv and any(
            (args.csv / name).resolve() == args.trace_path.resolve()
            for name in (
                "summary.csv",
                "sizes.csv",
                "models.csv",
                "memory.csv",
                "api_timings.csv",
            )
        ):
            raise ValueError("CSV output must not overwrite trace")
        config = TraceConfig(
            args.trace_path,
            tuple(args.model_block_size),
            args.lines,
            args.timestamp_unit,
        )
        manager, execution = manager_configuration(args), run_configuration(
            args
        )
        with prepare_trace(config) as workload:
            report = run_simulation(workload, manager, execution)
        report_output(report, args)
    except (ValueError, RuntimeError, OSError) as error:
        arguments.exit(2, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
