# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Replay the bundled conversations on one virtual timeline."""

from pathlib import Path

from meta_manager_sim.config import ManagerConfig, RunConfig
from meta_manager_sim.profiling.reporting import print_report
from meta_manager_sim.simulation import run_simulation
from meta_manager_sim.workloads.trace.trace_workload import (
    TraceConfig,
    prepare_trace,
)


def main() -> None:
    trace = TraceConfig(
        path=Path(__file__).with_name("traces.jsonl"),
        model_sizes=(
            ("example-model-a", 4 * 1024),
            ("example-model-b", 8 * 1024),
        ),
    )
    with prepare_trace(trace) as workload:
        report = run_simulation(
            workload,
            ManagerConfig(capacity_bytes=64 * 1024),
            RunConfig(
                execution="concurrent",
                workers=2,
                write_hold_ns=1_000_000,  # 1 ms of virtual time.
            ),
        )
    print_report(report)


if __name__ == "__main__":
    main()
