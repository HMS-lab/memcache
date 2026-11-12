# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Read shared keys with fixed sizes and native background eviction."""

from meta_manager_sim.config import ManagerConfig, RunConfig
from meta_manager_sim.profiling.reporting import print_report
from meta_manager_sim.simulation import run_simulation
from meta_manager_sim.workloads.random.random_workload import (
    RandomConfig,
    prepare_random,
)


def main() -> None:
    workload = prepare_random(
        RandomConfig(
            accesses=200,
            key_count=64,
            size_weights=((4 * 1024, 70), (8 * 1024, 30)),
            seed=42,
        )
    )
    report = run_simulation(
        workload,
        ManagerConfig(capacity_bytes=128 * 1024),
        RunConfig(sample_every=50),
    )
    print_report(report)


if __name__ == "__main__":
    main()
