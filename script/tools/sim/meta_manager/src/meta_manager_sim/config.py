# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Configuration shared by the workload drivers."""

from dataclasses import dataclass
from pathlib import Path

from meta_manager_sim.utils.sizes import check_size

TOOL = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class ManagerConfig:
    capacity_bytes: int = 64 << 20
    low_watermark_percent: int = 80
    high_watermark_percent: int = 90
    eviction_policy: str = "lru"
    lru_shards: int = 16
    lease_ttl_ms: int = 1000
    cleanup_timeout_ms: int = 5000
    max_value_alignment: bool = False

    def __post_init__(self) -> None:
        check_size(self.capacity_bytes)
        values = (
            self.low_watermark_percent,
            self.high_watermark_percent,
            self.lru_shards,
            self.lease_ttl_ms,
            self.cleanup_timeout_ms,
        )
        if any(type(value) is not int for value in values):
            raise ValueError("Manager counts and durations must be integers")
        if (
            not 0
            <= self.low_watermark_percent
            < self.high_watermark_percent
            <= 100
        ):
            raise ValueError("Require 0 <= min water level < max <= 100.")
        if self.eviction_policy not in ("lru", "sharded_lru"):
            raise ValueError("Eviction policy must be lru or sharded_lru.")
        if not (
            1 <= self.lru_shards <= 1024
            and self.lru_shards & (self.lru_shards - 1) == 0
        ):
            raise ValueError(
                "LRU shard count must be a power of two in 1..1024."
            )
        if not (
            0 < self.lease_ttl_ms < 1 << 64
            and 0 < self.cleanup_timeout_ms < 1 << 63
        ):
            raise ValueError("Lease TTL and cleanup timeout must be positive.")


@dataclass(frozen=True)
class RunConfig:
    execution: str = "ordered"
    workers: int = 4
    read_hold_ns: int = 0
    write_hold_ns: int = 0
    sample_every: int = 0

    def __post_init__(self) -> None:
        if (
            self.execution not in ("ordered", "concurrent")
            or self.workers <= 0
        ):
            raise ValueError("Invalid execution mode or worker count.")
        if min(self.read_hold_ns, self.write_hold_ns, self.sample_every) < 0:
            raise ValueError(
                "Holds and sampling interval must be nonnegative."
            )
        if self.execution == "ordered" and (
            self.read_hold_ns or self.write_hold_ns
        ):
            raise ValueError("Modeled holds require concurrent execution.")
