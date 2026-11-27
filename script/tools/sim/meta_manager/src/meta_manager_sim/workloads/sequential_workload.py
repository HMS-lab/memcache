# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from collections.abc import Iterator


class SequentialWorkload:
    def reads(
        self, num_reads: int, block_size: int
    ) -> Iterator[tuple[str, int]]:
        for block_id in range(num_reads):
            yield f"sequential:{block_id}", block_size
