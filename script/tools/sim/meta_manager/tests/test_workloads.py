# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import json
from pathlib import Path

import pytest

from meta_manager_sim.workloads import (
    RandomWorkload,
    SequentialWorkload,
    TraceWorkload,
)


def test_seeded_random_reads_keep_each_keys_size() -> None:
    workload = RandomWorkload(seed=42)
    reads = list(workload.reads(100, 20, {4096: 3, 8192: 1}))
    assert reads == list(workload.reads(100, 20, {4096: 3, 8192: 1}))
    assert len(reads) == 100
    assert len(set(reads)) == len(dict(reads))
    assert {size for _, size in reads} == {4096, 8192}
    assert all(key.startswith("random:") for key, _ in reads)


@pytest.mark.parametrize("hot_probability", [0, 1])
def test_hot_and_cold_selection(hot_probability: int) -> None:
    reads = RandomWorkload().reads(
        100, 20, {4096: 1}, hot_read_probability=hot_probability
    )
    ids = [int(key.split(":")[1]) for key, _ in reads]
    assert all(block < 4 if hot_probability else block >= 4 for block in ids)


def test_single_random_block() -> None:
    assert (
        list(RandomWorkload().reads(3, 1, {4096: 1}, 1, 0))
        == [("random:0", 4096)] * 3
    )


def test_sequential_reads_order_and_size() -> None:
    workload = SequentialWorkload()
    assert list(workload.reads(3, 8192)) == [
        ("sequential:0", 8192),
        ("sequential:1", 8192),
        ("sequential:2", 8192),
    ]
    assert list(workload.reads(0, 8192)) == []


def test_trace_order_models_and_line_selection(tmp_path: Path) -> None:
    path = tmp_path / "trace.jsonl"
    trace = {
        "requests": [
            {"type": "s", "t": 2, "model": "a", "hash_ids": [2]},
            {
                "type": "subagent",
                "requests": [
                    {"type": "n", "t": 0.1, "model": "a", "hash_ids": [1, 1]},
                    {"type": "s", "t": 2, "model": "b", "hash_ids": [1]},
                ],
            },
        ]
    }
    path.write_text(json.dumps({"requests": []}) + "\n" + json.dumps(trace))
    assert list(TraceWorkload(path).reads({"a": 4096, "b": 8192})) == []
    assert list(TraceWorkload(path, line=1).reads({"a": 4096, "b": 8192})) == [
        ("a:1", 4096),
        ("a:1", 4096),
        ("a:2", 4096),
        ("b:1", 8192),
    ]
    with pytest.raises(KeyError, match="Unknown block size for b"):
        list(TraceWorkload(path, line=1).reads({"a": 4096}))
