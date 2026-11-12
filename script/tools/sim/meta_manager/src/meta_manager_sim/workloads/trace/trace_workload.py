# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Normalize one conversation per logical source."""

from collections.abc import Generator, Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

from meta_manager_sim.utils.sizes import check_size
from meta_manager_sim.workloads.types import Access, Workload, WorkloadInfo

from .trace_reader import LineSelection, PreparedTrace


@dataclass(frozen=True)
class TraceConfig:
    path: Path
    model_sizes: tuple[tuple[str, int], ...]
    lines: LineSelection = LineSelection()
    timestamp_unit: str = "s"

    def __post_init__(self) -> None:
        names = [model for model, _ in self.model_sizes]
        if len(set(names)) != len(names) or any(not name for name in names):
            raise ValueError("Model mappings must be unique and nonempty.")
        if self.timestamp_unit not in ("s", "ms", "us", "ns"):
            raise ValueError("Invalid timestamp unit.")
        for _, size in self.model_sizes:
            check_size(size)


def block_key(model: str, block_id: int) -> str:
    return f"{model}:{block_id}"


def _source(
    trace: PreparedTrace, sizes: dict[str, int], line: int
) -> Generator[Access, None, None]:
    for request in trace.requests(line):
        for ordinal, block in enumerate(request["blocks"]):
            yield Access(
                block_key(request["model"], block),
                sizes[request["model"]],
                request["timestamp"],
                line,
                request["ordinal"],
                ordinal,
                request["model"],
            )


@contextmanager
def prepare_trace(config: TraceConfig) -> Iterator[Workload]:
    sizes = dict(config.model_sizes)
    with PreparedTrace(
        config.path, config.lines, sizes, config.timestamp_unit
    ) as trace:
        sources = tuple(_source(trace, sizes, line) for line in trace.lines)
        try:
            yield Workload(
                sources,
                WorkloadInfo(
                    "trace",
                    trace.accesses,
                    len(sources),
                    tuple(trace.lines),
                    trace.digest.hexdigest(),
                    "model_hash",
                    True,
                    config.timestamp_unit,
                    config,
                ),
            )
        finally:
            for source in sources:
                source.close()
