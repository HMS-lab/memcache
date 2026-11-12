# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Normalize conversation JSONL and merge timelines using a shared spool.

Adapted from the sibling allocator simulator's bounded-memory reader.
"""

import hashlib
import heapq
import json
import re
import tempfile
from collections.abc import Iterator
from dataclasses import dataclass
from decimal import ROUND_HALF_EVEN, Decimal
from pathlib import Path
from types import TracebackType
from typing import Any, BinaryIO, TypedDict

from meta_manager_sim.config import TOOL


class TraceRequest(TypedDict):
    timestamp: int
    line: int
    ordinal: int
    model: str
    blocks: list[int]


@dataclass(frozen=True)
class LineSelection:
    intervals: tuple[tuple[int, int], ...] = ()

    def contains(self, line: int) -> bool:
        return not self.intervals or any(
            start <= line <= end for start, end in self.intervals
        )

    @property
    def last(self) -> int | None:
        return self.intervals[-1][1] if self.intervals else None


def parse_line_selection(text: str) -> LineSelection:
    if text == "all":
        return LineSelection()
    intervals = []
    for item in text.split(","):
        match = re.fullmatch(r"\s*([1-9]\d*)(?::([1-9]\d*))?\s*", item)
        if match is None:
            raise ValueError(
                "Use all or positive line numbers and A:B ranges."
            )
        start, end = int(match[1]), int(match[2] or match[1])
        if end < start:
            raise ValueError("Line ranges must have increasing endpoints.")
        intervals.append((start, end))
    merged: list[tuple[int, int]] = []
    for start, end in sorted(intervals):
        if merged and start <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return LineSelection(tuple(merged))


def request_records(
    conversation: dict[str, Any],
    line: int,
    model_sizes: dict[str, int],
    timestamp_unit: str = "s",
) -> Iterator[TraceRequest]:
    if conversation.get("hash_id_scope") != "local":
        raise ValueError("Only hash_id_scope: local is supported.")
    if (
        type(conversation.get("block_size")) is not int
        or conversation["block_size"] <= 0
    ):
        raise ValueError("block_size must be a positive token count.")
    requests = conversation["requests"]
    if not isinstance(requests, list):
        raise ValueError("requests must be a list.")
    stack = list(reversed(requests))
    ordinal = 0
    while stack:
        request = stack.pop()
        kind = request["type"]
        if kind == "subagent":
            children = request["requests"]
            if not isinstance(children, list):
                raise ValueError("Subagent requests must be a list.")
            stack.extend(reversed(children))
            continue
        if kind not in ("s", "n"):
            raise ValueError(f"Unsupported request type: {kind!r}.")
        timestamp, model, blocks = (
            request["t"],
            request["model"],
            request["hash_ids"],
        )
        if (
            type(timestamp) not in (int, Decimal)
            or not Decimal(timestamp).is_finite()
            or timestamp < 0
        ):
            raise ValueError("Request time must be finite and nonnegative.")
        if model not in model_sizes:
            raise ValueError(f"Missing block byte size for model {model!r}.")
        if not isinstance(blocks, list) or any(
            type(block) is not int or block < 0 for block in blocks
        ):
            raise ValueError(
                "hash_ids must be a list of nonnegative integers."
            )
        normalized = int(
            (
                Decimal(timestamp)
                * {"s": 10**9, "ms": 10**6, "us": 1000, "ns": 1}[
                    timestamp_unit
                ]
            ).to_integral_value(rounding=ROUND_HALF_EVEN)
        )
        if normalized > (1 << 64) - 1:
            raise ValueError("Timestamp exceeds unsigned 64-bit nanoseconds")
        ordinal += 1
        yield {
            "timestamp": normalized,
            "line": line,
            "ordinal": ordinal,
            "model": model,
            "blocks": blocks,
        }


def request_order(record: TraceRequest) -> tuple[int, int, int]:
    return record["timestamp"], record["line"], record["ordinal"]


class PreparedTrace:
    """Keep one conversation in memory while preparing sorted disk spans."""

    def __init__(
        self,
        path: Path,
        selection: LineSelection,
        model_sizes: dict[str, int],
        timestamp_unit: str = "s",
    ) -> None:
        self.timestamp_unit = timestamp_unit
        self.digest = hashlib.sha256()
        self.path = Path(path)
        self.selection = selection
        self.model_sizes = model_sizes
        self.lines: list[int] = []
        self.request_count = 0
        self.accesses = 0
        self._spans: list[tuple[int, int]] = []

    def __enter__(self) -> "PreparedTrace":
        directory = TOOL / "build"
        directory.mkdir(exist_ok=True)
        self._temporary = tempfile.TemporaryDirectory(
            prefix="trace-", dir=directory
        )
        self._spool = Path(self._temporary.name) / "requests.jsonl"
        try:
            self._prepare()
        except BaseException:
            self._temporary.cleanup()
            raise
        self._stream = self._spool.open("rb")
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self._stream.close()
        self._temporary.cleanup()

    def requests(self, line: int) -> Iterator[TraceRequest]:
        start, end = self._spans[self.lines.index(line)]
        yield from self._records(self._stream, start, end)

    def _prepare(self) -> None:
        number = 0
        with (
            self.path.open(encoding="utf-8") as source,
            self._spool.open("wb") as target,
        ):
            for number, text in enumerate(source, 1):
                if self.selection.contains(number):
                    try:
                        records = sorted(
                            request_records(
                                json.loads(text, parse_float=Decimal),
                                number,
                                self.model_sizes,
                                self.timestamp_unit,
                            ),
                            key=request_order,
                        )
                    except (
                        ValueError,
                        KeyError,
                        TypeError,
                        AttributeError,
                        OverflowError,
                    ) as error:
                        raise ValueError(f"Line {number}: {error}") from error
                    self.digest.update(
                        number.to_bytes(8, "big")
                        + len(text.encode()).to_bytes(8, "big")
                        + text.encode()
                    )
                    self.lines.append(number)
                    start = target.tell()
                    for record in records:
                        target.write((json.dumps(record) + "\n").encode())
                        self.request_count += 1
                        self.accesses += len(record["blocks"])
                    self._spans.append((start, target.tell()))
                if number == self.selection.last:
                    break
        if self.selection.last is not None and number < self.selection.last:
            raise ValueError(
                f"Line {self.selection.last} is outside the file "
                f"({number} lines)."
            )
        if not self.lines:
            raise ValueError("No conversations were selected.")

    @staticmethod
    def _records(
        stream: BinaryIO, start: int, end: int
    ) -> Iterator[TraceRequest]:
        position = start
        while position < end:
            stream.seek(position)
            record = json.loads(stream.readline())
            position = stream.tell()
            yield record

    def __iter__(self) -> Iterator[TraceRequest]:
        with self._spool.open("rb") as stream:
            timelines = [
                self._records(stream, start, end) for start, end in self._spans
            ]
            yield from heapq.merge(*timelines, key=request_order)
