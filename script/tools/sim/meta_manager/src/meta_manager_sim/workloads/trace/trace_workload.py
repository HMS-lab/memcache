# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from collections.abc import Iterator
from pathlib import Path

from .trace_reader import TraceReader


class TraceWorkload:
    def __init__(self, filepath: Path | str, line: int = 0):
        self._reader = TraceReader(filepath, line)

    def reads(
        self, model_block_sizes: dict[str, int]
    ) -> Iterator[tuple[str, int]]:
        for model, block_id in self._reader.requests():
            try:
                size = model_block_sizes[model]
                yield f"{model}:{block_id}", size
            except KeyError as e:
                raise KeyError(f"Unknown block size for {model}") from e
