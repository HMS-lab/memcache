# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Seeded shared-key sources with stable per-key sizes."""

import math
import random
from collections.abc import Iterator
from dataclasses import dataclass

from meta_manager_sim.utils.sizes import check_size
from meta_manager_sim.workloads.types import Access, Workload, WorkloadInfo


@dataclass(frozen=True)
class RandomConfig:
    accesses: int = 10000
    key_count: int = 10000
    size_weights: tuple[tuple[int, float], ...] = (
        (4096, 60),
        (16384, 30),
        (65536, 10),
    )
    source_count: int = 1
    access_interval_ns: int = 1000000
    hot_key_fraction: float = 0.2
    hot_read_probability: float = 0.8
    seed: int = 42

    def __post_init__(self) -> None:
        if not 0 < self.source_count <= self.accesses or self.key_count <= 0:
            raise ValueError(
                "Require 0 < sources <= accesses and positive key count."
            )
        if self.access_interval_ns < 0:
            raise ValueError("Arrival interval must be nonnegative.")
        if (
            not 0 < self.hot_key_fraction <= 1
            or not 0 <= self.hot_read_probability <= 1
        ):
            raise ValueError("Invalid hot/cold probabilities.")
        if not self.size_weights:
            raise ValueError("Supply weighted sizes.")
        for size, weight in self.size_weights:
            check_size(size)
            if not math.isfinite(weight) or weight <= 0:
                raise ValueError("Weights must be finite and positive.")


def _source(
    config: RandomConfig, sizes: tuple[int, ...], source: int
) -> Iterator[Access]:
    reads = random.Random(f"{config.seed}:reads:{source}")
    hot = max(1, int(config.key_count * config.hot_key_fraction))
    for index in range(source, config.accesses, config.source_count):
        if (
            hot == config.key_count
            or reads.random() < config.hot_read_probability
        ):
            key = reads.randrange(hot)
        else:
            key = reads.randrange(hot, config.key_count)
        yield Access(
            f"random/{key}",
            sizes[key],
            index // config.source_count * config.access_interval_ns,
            source,
            index,
        )


def prepare_random(config: RandomConfig) -> Workload:
    weights: dict[int, float] = {}
    for size, weight in config.size_weights:
        weights[size] = weights.get(size, 0) + weight
    sizes = tuple(
        random.Random(f"{config.seed}:sizes").choices(
            tuple(weights), tuple(weights.values()), k=config.key_count
        )
    )
    return Workload(
        tuple(
            _source(config, sizes, source)
            for source in range(config.source_count)
        ),
        WorkloadInfo(
            "random",
            config.accesses,
            config.source_count,
            configuration=config,
        ),
    )
