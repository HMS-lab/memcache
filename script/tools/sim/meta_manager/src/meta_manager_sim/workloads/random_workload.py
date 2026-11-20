# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from collections.abc import Iterator

from .tools import RandomGenerator


class RandomWorkload:
    def __init__(self, seed: int = 0):
        self._generator = RandomGenerator(seed)

    def reads(
        self,
        num_reads: int,
        num_blocks: int,
        size_weights: dict[int, int],
        hot_block_fraction: float = 0.2,
        hot_read_probability: float = 0.8,
    ) -> Iterator[tuple[str, int]]:
        for block_id, size in self._generator.requests(
            num_reads,
            num_blocks,
            size_weights,
            hot_block_fraction,
            hot_read_probability,
        ):
            yield f"random:{block_id}", size
