# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Single-use input streams; native code alone owns residency."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .random.random_workload import RandomConfig
    from .trace.trace_workload import TraceConfig

from collections.abc import Iterator
from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class Access:
    key: str
    size_bytes: int
    at_ns: int = 0
    source_id: int = 0
    request_ordinal: int = 0
    block_ordinal: int = 0
    model: str | None = None


@dataclass(frozen=True)
class WorkloadInfo:
    kind: str
    accesses: int
    sources: int
    selected_lines: tuple[int, ...] = ()
    selected_input_sha256: str | None = None
    effective_key_scope: str = "shared_catalog"
    key_scope_override: bool = False
    timestamp_unit: str = "ns"
    configuration: RandomConfig | TraceConfig | None = None


@dataclass(frozen=True)
class Workload:
    sources: tuple[Iterator[Access], ...]
    info: WorkloadInfo
