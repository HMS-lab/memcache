# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import random
from collections.abc import Iterator


class RandomGenerator:
    def __init__(self, seed: int = 0) -> None:
        self._seed = seed

    def requests(
        self,
        num_reads: int,
        num_blocks: int,
        size_weights: dict[int, int],
        hot_block_fraction: float = 0.2,
        hot_read_probability: float = 0.8,
    ) -> Iterator[tuple[int, int]]:
        # Reset the seeded generator for reproducible calls
        rng = random.Random(self._seed)

        # Define the hot block range and assign each block a fixed size
        num_hot_blocks = max(1, int(num_blocks * hot_block_fraction))
        sizes = rng.choices(
            list(size_weights), list(size_weights.values()), k=num_blocks
        )

        # Note: The first condition avoids choosing an empty cold range
        for _ in range(num_reads):
            block_id = (
                rng.randrange(num_hot_blocks)
                if num_hot_blocks == num_blocks
                or rng.random() < hot_read_probability
                else rng.randrange(num_hot_blocks, num_blocks)
            )
            yield block_id, sizes[block_id]
