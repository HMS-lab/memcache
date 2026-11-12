# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Recorded results have no dependency on the compiled adapter."""

from dataclasses import dataclass
from typing import Any, Literal, TypedDict


@dataclass(frozen=True, slots=True)
class Blob:
    rank: int
    media: int
    size: int
    reserved: int
    state: int = 0
    remaining_lease_ttl_ms: int = 0
    gva: int = 0


@dataclass(frozen=True, slots=True)
class OperationResult:
    key: str
    outcome: str
    code: int
    operation_id: int
    elapsed_ns: int
    blobs: tuple[Blob, ...] = ()
    size_bytes: int = 0
    model: str | None = None
    check_elapsed_ns: int = 0


@dataclass(frozen=True, slots=True)
class PoolSnapshot:
    rank: int
    media: str
    capacity: int
    reserved: int
    internal_bytes: int


@dataclass(frozen=True, slots=True)
class Snapshot:
    accesses: int
    keys: int
    reclaimed_blobs: int
    pools: tuple[PoolSnapshot, ...]
    virtual_time_ns: int = 0
    reason: str = "periodic"
    requested_threshold: int | None = None


@dataclass(frozen=True, slots=True)
class LeaseToken:
    result: OperationResult
    kind: str
    deadline_ns: int


@dataclass(frozen=True, slots=True)
class StartedAccess:
    lookup: OperationResult
    allocation: OperationResult | None
    token: LeaseToken | None


class SimulationResult(TypedDict):
    """Detached report payload; JSON conversion is confined to reporting."""

    schema_version: int
    status: Literal["completed"]
    workload: dict[str, Any]
    configuration: dict[str, Any]
    provenance: dict[str, str]
    metrics: dict[str, Any]
    samples: list[dict[str, Any]]
