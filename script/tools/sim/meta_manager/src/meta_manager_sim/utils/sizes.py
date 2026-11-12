# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Configuration shared by the workload drivers."""

import re

# Leave room for native alignment and the pool's 4 KiB base address.
MAX_SIZE = (1 << 64) - 1 - 4096
UNITS = {"": 1, "b": 1, "kib": 1 << 10, "mib": 1 << 20, "gib": 1 << 30}


def byte_size(text: str) -> int:
    match = re.fullmatch(r"\s*(\d+)\s*(B|KiB|MiB|GiB)?\s*", text, re.I)
    if match is None:
        raise ValueError("Use bytes or an integer with B, KiB, MiB, or GiB.")
    size = int(match[1]) * UNITS[(match[2] or "").lower()]
    check_size(size)
    return size


def check_size(size: int) -> None:
    if type(size) is not int or not 0 < size <= MAX_SIZE:
        raise ValueError(f"Byte sizes must be between 1 and {MAX_SIZE}.")
