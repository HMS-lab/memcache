# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from pathlib import Path


def read_rss(pid: int) -> int:
    # Note: Linux reports RSS in KiB despite the kB label
    with Path(f"/proc/{pid}/smaps_rollup").open() as file:
        for line in file:
            if line.startswith("Rss:"):
                return int(line.split()[1]) * 1024

    raise ValueError("Missing Rss in smaps_rollup")
