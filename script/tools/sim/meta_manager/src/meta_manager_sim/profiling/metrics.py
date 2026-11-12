# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Operation accounting and pure calculations from native observations."""

from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from typing import Any

from meta_manager_sim.profiling.models import (
    OperationResult,
    PoolSnapshot,
    Snapshot,
)


def percentage(numerator: int, denominator: int) -> float | None:
    return 100 * numerator / denominator if denominator else None


@dataclass(slots=True)
class ApiTiming:
    calls: int = 0
    total_ns: int = 0
    max_ns: int = 0
    min_ns: int | None = None

    def add(self, elapsed_ns: int) -> None:
        if self.min_ns is None or elapsed_ns < self.min_ns:
            self.min_ns = elapsed_ns
        self.calls += 1
        self.total_ns += elapsed_ns
        if elapsed_ns > self.max_ns:
            self.max_ns = elapsed_ns


class Metrics:
    def __init__(self) -> None:
        self.native_statuses: Counter[tuple[str, int]] = Counter()
        self.by_model: defaultdict[str, Counter[str]] = defaultdict(Counter)
        self.cumulative_allocation_reserved_bytes = 0
        self.outcomes: Counter[str] = Counter()
        self.by_size: defaultdict[int, Counter[str]] = defaultdict(Counter)
        self.timings: defaultdict[str, ApiTiming] = defaultdict(ApiTiming)

    def record(self, operation: str, result: OperationResult) -> None:
        outcome = f"{operation}.{result.outcome}"
        self.native_statuses[(operation, result.code)] += 1
        if result.model is not None:
            self.by_model[result.model][outcome] += 1
        if operation == "alloc":
            self.timings["watermark_check"].add(result.check_elapsed_ns)
            if result.outcome == "allocated":
                self.cumulative_allocation_reserved_bytes += sum(
                    blob.reserved for blob in result.blobs
                )
        self.outcomes[outcome] += 1
        self.by_size[result.size_bytes][outcome] += 1
        self.timings[operation].add(result.elapsed_ns)

    @property
    def accesses(self) -> int:
        return sum(
            self.outcomes[f"get.{outcome}"]
            for outcome in ("hit", "missing", "unreadable")
        )

    def validate(self) -> None:
        counts = self.outcomes
        misses = counts["get.missing"] + counts["get.unreadable"]
        attempts = sum(
            counts[f"alloc.{outcome}"]
            for outcome in ("allocated", "duplicate", "failed")
        )
        if (
            misses != attempts
            or counts["finish_read.ok"] != counts["get.hit"]
            or counts["finish_write.ok"] != counts["alloc.allocated"]
        ):
            raise RuntimeError("Operation accounting is inconsistent")

    def summary(self) -> dict[str, Any]:
        byte_counts = {
            "requested_access_bytes": "accesses",
            "cache_hit_bytes": "cache_hits",
            "cache_miss_bytes": "cache_misses",
            "allocation_attempted_bytes": "allocation_attempts",
            "allocation_success_bytes": "allocation_successes",
            "allocation_failure_bytes": "allocation_failures",
            "allocation_duplicate_bytes": "allocation_duplicates",
        }
        byte_totals: Counter[str] = Counter(dict.fromkeys(byte_counts, 0))
        rows = []
        for size, counts in sorted(self.by_size.items()):
            outcomes = _outcome_metrics(counts)
            byte_metrics = {
                name: size * outcomes[counter]
                for name, counter in byte_counts.items()
            }
            byte_totals.update(byte_metrics)
            rows.append({"size_bytes": size, **outcomes, **byte_metrics})

        padding = (
            self.cumulative_allocation_reserved_bytes
            - byte_totals["allocation_success_bytes"]
        )
        return {
            **_outcome_metrics(self.outcomes),
            **byte_totals,
            "native_statuses": [
                {"api": operation, "code": code, "calls": count}
                for (operation, code), count in sorted(
                    self.native_statuses.items()
                )
            ],
            "by_size": rows,
            "by_model": [
                {
                    "model": model,
                    **_outcome_metrics(counts),
                    "outcomes": dict(counts),
                }
                for model, counts in sorted(self.by_model.items())
            ],
            "cumulative_allocation_reserved_bytes": (
                self.cumulative_allocation_reserved_bytes
            ),
            "cumulative_allocation_padding_bytes": padding,
            "cumulative_allocation_padding_percent": percentage(
                padding, self.cumulative_allocation_reserved_bytes
            ),
            "byte_hit_rate_percent": percentage(
                byte_totals["cache_hit_bytes"],
                byte_totals["requested_access_bytes"],
            ),
            "outcomes": dict(sorted(self.outcomes.items())),
            "api_timing": {
                name: {
                    **asdict(timing),
                    "mean_ns": timing.total_ns / timing.calls,
                    "total_seconds": timing.total_ns / 1_000_000_000,
                }
                for name, timing in sorted(self.timings.items())
            },
        }


def _outcome_metrics(counts: Counter[str]) -> dict[str, Any]:
    hits = counts["get.hit"]
    missing = counts["get.missing"]
    unreadable = counts["get.unreadable"]
    misses = missing + unreadable
    accesses = hits + misses
    successes = counts["alloc.allocated"]
    failures = counts["alloc.failed"]
    duplicates = counts["alloc.duplicate"]
    attempts = successes + failures + duplicates

    return {
        "accesses": accesses,
        "cache_hits": hits,
        "cache_misses": misses,
        "missing_key_misses": missing,
        "unreadable_key_misses": unreadable,
        "cache_hit_rate_percent": percentage(hits, accesses),
        "cache_miss_rate_percent": percentage(misses, accesses),
        "allocation_attempts": attempts,
        "allocation_successes": successes,
        "allocation_failures": failures,
        "allocation_duplicates": duplicates,
        "allocation_success_percent": percentage(successes, attempts),
        "allocation_failure_percent": percentage(failures, attempts),
        "allocation_duplicate_percent": percentage(duplicates, attempts),
        "read_completions": counts["finish_read.ok"],
        "write_completions": counts["finish_write.ok"],
    }


def pool_metrics(pool: PoolSnapshot) -> dict[str, int | float | str | None]:
    return {
        "rank": pool.rank,
        "media": pool.media,
        "capacity_bytes": pool.capacity,
        "reserved_bytes": pool.reserved,
        "free_bytes": pool.capacity - pool.reserved,
        "padding_bytes": pool.internal_bytes,
        "live_requested_bytes": pool.reserved - pool.internal_bytes,
        "occupancy_percent": percentage(pool.reserved, pool.capacity),
        "padding_percent": percentage(pool.internal_bytes, pool.reserved),
    }


def snapshot_metrics(snapshot: Snapshot) -> dict[str, Any]:
    return {
        "accesses": snapshot.accesses,
        "registered_keys": snapshot.keys,
        "virtual_time_ns": snapshot.virtual_time_ns,
        "reason": snapshot.reason,
        "requested_threshold": snapshot.requested_threshold,
        "reclaimed_blobs": snapshot.reclaimed_blobs,
        "pools": [pool_metrics(pool) for pool in snapshot.pools],
    }
