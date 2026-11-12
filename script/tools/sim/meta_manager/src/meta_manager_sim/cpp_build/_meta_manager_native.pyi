# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Types for the extension built and loaded by cpp_build/build.py."""

from typing import ClassVar

OK: int
MISSING: int
DUPLICATE: int
ALLOCATION_FAILED: int

class Blob:
    gva: int
    rank: int
    media: int
    size: int
    reserved: int
    state: int
    remaining_lease_ttl_ms: int

class Descriptor:
    num_blobs: int
    size: int
    blobs: list[Blob]

class OperationResult:
    code: int
    operation_id: int
    elapsed_ns: int
    descriptor: Descriptor

class Action:
    WRITE_OK: ClassVar[Action]
    READ_FINISH: ClassVar[Action]

class Manager:
    def __init__(
        self,
        capacity: int,
        high: int,
        low: int,
        lease_ttl_ms: int,
        max_value_alignment: bool,
        policy: str,
        shards: int,
        /,
    ) -> None: ...
    def alloc(self, key: str, size: int, /) -> OperationResult: ...
    def get(self, key: str, /) -> OperationResult: ...
    def update(
        self,
        key: str,
        operation_id: int,
        rank: int,
        media: int,
        action: Action,
        /,
    ) -> OperationResult: ...
    def remove(self, key: str, /) -> OperationResult: ...
    def check_and_evict(self, wanted_size: int) -> OperationResult: ...
    def wait_for_cleanup(self, timeout_ms: int, /) -> bool: ...
    def segment_info(self) -> str: ...
    def key_count(self) -> int: ...
    def keys(self) -> list[str]: ...
    @property
    def peak_calls(self) -> int: ...
    @property
    def reclaimed(self) -> int: ...
    def close(self) -> None: ...
