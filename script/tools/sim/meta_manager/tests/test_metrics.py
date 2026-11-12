# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Accounting stays complete when aggregation reuses existing groups."""

import pytest

from meta_manager_sim.profiling.metrics import ApiTiming, Metrics, pool_metrics
from meta_manager_sim.profiling.models import (
    Blob,
    OperationResult,
    PoolSnapshot,
)

MISSING_CODE = -10
FAILED_CODE = -11
DUPLICATE_CODE = -12


def test_timing_tracks_zero_minimum_and_extrema() -> None:
    timing = ApiTiming()
    for elapsed in (0, 6, 2):
        timing.add(elapsed)
    assert timing.calls == 3
    assert timing.total_ns == 8
    assert timing.min_ns == 0
    assert timing.max_ns == 6


def test_metrics_preserve_sizes_models_statuses_and_timings() -> None:
    metrics = Metrics()
    events = (
        ("get", "missing", MISSING_CODE, 4096, "alpha", 10, 0),
        ("alloc", "allocated", 0, 4096, "alpha", 30, 3),
        ("finish_write", "ok", 0, 4096, "alpha", 8, 0),
        ("get", "hit", 0, 4096, "alpha", 12, 0),
        ("finish_read", "ok", 0, 4096, "alpha", 4, 0),
        ("get", "missing", MISSING_CODE, 8192, "beta", 11, 0),
        ("alloc", "failed", FAILED_CODE, 8192, "beta", 20, 3),
        ("get", "unreadable", 0, 8192, "beta", 15, 0),
        ("alloc", "duplicate", DUPLICATE_CODE, 8192, "beta", 25, 4),
    )
    for operation, outcome, code, size, model, elapsed, check in events:
        blobs = (Blob(0, 0, size, 8192),) if outcome == "allocated" else ()
        metrics.record(
            operation,
            OperationResult(
                key=model,
                outcome=outcome,
                code=code,
                operation_id=1,
                elapsed_ns=elapsed,
                blobs=blobs,
                size_bytes=size,
                model=model,
                check_elapsed_ns=check,
            ),
        )

    metrics.validate()
    result = metrics.summary()
    assert result["accesses"] == 4
    assert result["cache_hits"] == 1
    assert result["cache_misses"] == 3
    assert result["missing_key_misses"] == 2
    assert result["unreadable_key_misses"] == 1
    assert result["cache_hit_rate_percent"] == 25
    assert result["cache_miss_rate_percent"] == 75
    assert result["byte_hit_rate_percent"] == pytest.approx(100 / 6)
    assert result["requested_access_bytes"] == 24576
    assert result["cache_hit_bytes"] == 4096
    assert result["cache_miss_bytes"] == 20480
    assert result["allocation_attempts"] == 3
    assert result["allocation_successes"] == 1
    assert result["allocation_failures"] == 1
    assert result["allocation_duplicates"] == 1
    assert result["allocation_success_percent"] == pytest.approx(100 / 3)
    assert result["allocation_failure_percent"] == pytest.approx(100 / 3)
    assert result["allocation_duplicate_percent"] == pytest.approx(100 / 3)
    assert result["allocation_attempted_bytes"] == 20480
    assert result["allocation_success_bytes"] == 4096
    assert result["allocation_failure_bytes"] == 8192
    assert result["allocation_duplicate_bytes"] == 8192
    assert result["cumulative_allocation_reserved_bytes"] == 8192
    assert result["cumulative_allocation_padding_bytes"] == 4096
    assert result["cumulative_allocation_padding_percent"] == 50
    rows = {row["size_bytes"]: row for row in result["by_size"]}
    assert rows[4096]["allocation_successes"] == 1
    assert rows[4096]["cache_hit_rate_percent"] == 50
    assert rows[4096]["requested_access_bytes"] == 8192
    assert rows[8192]["allocation_attempts"] == 2
    assert rows[8192]["allocation_failure_bytes"] == 8192
    assert rows[8192]["allocation_duplicate_bytes"] == 8192
    assert rows[8192]["allocation_failure_percent"] == 50
    models = {row["model"]: row for row in result["by_model"]}
    assert models["alpha"]["cache_hit_rate_percent"] == 50
    assert models["alpha"]["allocation_success_percent"] == 100
    assert models["alpha"]["read_completions"] == 1
    assert models["alpha"]["write_completions"] == 1
    assert models["beta"]["allocation_failure_percent"] == 50
    assert models["beta"]["allocation_duplicate_percent"] == 50
    assert models["beta"]["outcomes"]["alloc.failed"] == 1
    assert models["beta"]["outcomes"]["alloc.duplicate"] == 1
    assert result["api_timing"]["get"] == {
        "calls": 4,
        "total_ns": 48,
        "min_ns": 10,
        "max_ns": 15,
        "mean_ns": 12,
        "total_seconds": 48 / 1_000_000_000,
    }
    assert result["api_timing"]["watermark_check"]["total_ns"] == 10
    assert {
        (row["api"], row["code"], row["calls"])
        for row in result["native_statuses"]
    } == {
        ("get", MISSING_CODE, 2),
        ("get", 0, 2),
        ("alloc", 0, 1),
        ("alloc", FAILED_CODE, 1),
        ("alloc", DUPLICATE_CODE, 1),
        ("finish_read", 0, 1),
        ("finish_write", 0, 1),
    }


def test_empty_metrics_do_not_create_groups_or_rates() -> None:
    metrics = Metrics()
    metrics.validate()
    result = metrics.summary()
    assert result["by_size"] == []
    assert result["by_model"] == []
    assert result["api_timing"] == {}
    assert result["outcomes"] == {}
    assert result["cache_hit_rate_percent"] is None
    assert result["cache_miss_rate_percent"] is None
    assert result["byte_hit_rate_percent"] is None
    assert result["allocation_success_percent"] is None
    assert result["allocation_failure_percent"] is None
    assert result["allocation_duplicate_percent"] is None
    assert result["cumulative_allocation_padding_percent"] is None
    assert result["cache_miss_bytes"] == 0
    assert result["allocation_failures"] == 0
    assert result["cumulative_allocation_reserved_bytes"] == 0
    assert result["cumulative_allocation_padding_bytes"] == 0


def test_pool_padding_is_separate_from_free_memory() -> None:
    result = pool_metrics(PoolSnapshot(0, "DRAM", 65536, 8192, 3192))
    assert result["capacity_bytes"] == 65536
    assert result["reserved_bytes"] == 8192
    assert result["live_requested_bytes"] == 5000
    assert result["padding_bytes"] == 3192
    assert result["free_bytes"] == 57344
    assert result["padding_percent"] == pytest.approx(100 * 3192 / 8192)
    assert result["occupancy_percent"] == 12.5
    empty = pool_metrics(PoolSnapshot(0, "DRAM", 65536, 0, 0))
    assert empty["padding_percent"] is None
    assert empty["occupancy_percent"] == 0
