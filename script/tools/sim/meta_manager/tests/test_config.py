# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from argparse import ArgumentParser
from pathlib import Path

from meta_manager_sim.cli import add_common_arguments
from meta_manager_sim.cli.parsers import (
    key_and_size,
    keys_and_sizes,
    sizes_and_values,
)
from meta_manager_sim.config import MetaManagerConfig, RunConfig


def test_sizes_and_repeated_groups() -> None:
    assert key_and_size("model:4096B") == ("model", 4096)
    assert keys_and_sizes("10:40GiB,20:100GiB,500:1024MiB") == {
        "10": 40 << 30,
        "20": 100 << 30,
        "500": 1 << 30,
    }
    assert keys_and_sizes("2:4KiB,2:8KiB") == {"2": 12288}
    assert sizes_and_values("4KiB:1,4096:2,8KiB:3") == {4096: 3, 8192: 3}


def test_from_args() -> None:
    parser = ArgumentParser()
    add_common_arguments(parser)
    args = parser.parse_args(
        [
            "--pools",
            "2:8MiB,1:40GiB",
            "--lease-ttl-ms",
            "2500",
            "--min-water-level",
            "70",
            "--max-water-level",
            "95",
            "--eviction-policy",
            "lru",
            "--lru-shards",
            "8",
            "--memory-sample-every",
            "100",
            "--memory-plot",
            "build/test.png",
            "--detailed-stats",
            "--profile",
        ]
    )
    assert RunConfig.from_args(args) == RunConfig(
        manager_cfg=MetaManagerConfig(
            pools=((2, 8 << 20), (1, 40 << 30)),
            lease_ttl_ms=2500,
            min_water_level=70,
            max_water_level=95,
            eviction_policy="lru",
            lru_shards=8,
        ),
        memory_sample_every=100,
        memory_plot=Path("build/test.png"),
        detailed_stats=True,
        profile=True,
    )


def test_cli_defaults() -> None:
    parser = ArgumentParser()
    add_common_arguments(parser)
    assert RunConfig.from_args(parser.parse_args([])) == RunConfig(
        manager_cfg=MetaManagerConfig(pools=((1, 20 << 30),))
    )
