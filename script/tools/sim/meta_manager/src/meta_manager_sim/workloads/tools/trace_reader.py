# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import json
from collections.abc import Iterator
from itertools import islice
from pathlib import Path
from typing import Any


class TraceReader:
    def __init__(self, filepath: Path | str, line: int = 0):
        self._filepath = Path(filepath)
        self._line = line

    def _parse_requests(
        self, requests: list[dict[str, Any]]
    ) -> Iterator[tuple[float, str, list[int]]]:
        for request in requests:
            if request["type"] == "subagent":
                # Flatten nested subagent requests
                yield from self._parse_requests(request["requests"])
            else:
                yield request["t"], request["model"], request["hash_ids"]

    def requests(self) -> Iterator[tuple[str, int]]:
        # Skip preceding lines and read the selected entry
        with self._filepath.open("rb") as file:
            raw = next(islice(file, self._line, None))

        # Decode the entry and extract block requests (in timestamp order)
        requests = self._parse_requests(json.loads(raw)["requests"])
        for _, model, block_ids in sorted(
            requests, key=lambda request: request[0]
        ):
            for block_id in block_ids:
                yield model, block_id
