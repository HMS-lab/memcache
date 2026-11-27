# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.


def size_bytes(raw: str) -> int:
    # Note: Keep the common suffix 'B' at the very end
    units = {"GiB": 1 << 30, "MiB": 1 << 20, "KiB": 1 << 10, "B": 1}

    for unit, size in units.items():
        if raw.endswith(unit):
            return int(raw.removesuffix(unit)) * size

    return int(raw)


def key_and_size(raw: str) -> tuple[str, int]:
    key, size_str = raw.split(":")
    return key, size_bytes(size_str)


def keys_and_sizes(raw: str) -> dict[str, int]:
    result: dict[str, int] = {}

    # Note: Repeated keys contribute to one total size
    for group in raw.split(","):
        key, size = key_and_size(group)
        result[key] = result.get(key, 0) + size

    return result


def size_and_value(raw: str) -> tuple[int, int]:
    size_str, value_str = raw.split(":")
    return size_bytes(size_str), int(value_str)


def sizes_and_values(raw: str) -> dict[int, int]:
    result: dict[int, int] = {}

    # Note: Repeated sizes contribute to one total value
    for group in raw.split(","):
        size, value = size_and_value(group)
        result[size] = result.get(size, 0) + value

    return result
