# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from argparse import Namespace
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class MetaManagerConfig:
    pools: tuple[tuple[int, int], ...] = ((1, 64 << 20),)
    lease_ttl_ms: int = 10000
    min_water_level: int = 80
    max_water_level: int = 90
    eviction_policy: str = "sharded_lru"
    lru_shards: int = 16


@dataclass(frozen=True)
class RunConfig:
    manager_cfg: MetaManagerConfig = MetaManagerConfig()
    memory_sample_every: int = 1
    memory_plot: Path | None = None
    detailed_stats: bool = False
    profile: bool = False

    @staticmethod
    def from_args(args: Namespace) -> "RunConfig":
        return RunConfig(
            manager_cfg=MetaManagerConfig(
                pools=tuple((int(k), v) for k, v in dict(args.pools).items()),
                lease_ttl_ms=args.lease_ttl_ms,
                min_water_level=args.min_water_level,
                max_water_level=args.max_water_level,
                eviction_policy=args.eviction_policy,
                lru_shards=args.lru_shards,
            ),
            memory_sample_every=args.memory_sample_every,
            memory_plot=args.memory_plot,
            detailed_stats=args.detailed_stats,
            profile=args.profile,
        )
