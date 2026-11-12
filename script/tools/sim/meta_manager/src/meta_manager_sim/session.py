# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Native lifetime and metadata access; callers own completion tokens."""

import json
from time import monotonic_ns
from types import TracebackType
from typing import TYPE_CHECKING

from meta_manager_sim.config import ManagerConfig
from meta_manager_sim.cpp_build.build import (
    NativeBackend,
    claim_manager,
    release_manager,
)
from meta_manager_sim.profiling.models import (
    Blob,
    LeaseToken,
    OperationResult,
    PoolSnapshot,
    Snapshot,
    StartedAccess,
)
from meta_manager_sim.utils.sizes import check_size
from meta_manager_sim.workloads.types import Access

if TYPE_CHECKING:
    from meta_manager_sim.cpp_build._meta_manager_native import (
        OperationResult as NativeResult,
    )


class ManagerSession:
    def __init__(self, native: NativeBackend, config: ManagerConfig) -> None:
        claim_manager()
        self._closed = False
        self.native = native
        self.timeout = config.cleanup_timeout_ms
        try:
            self.manager = native.Manager(
                config.capacity_bytes,
                config.high_watermark_percent,
                config.low_watermark_percent,
                config.lease_ttl_ms,
                config.max_value_alignment,
                config.eviction_policy,
                config.lru_shards,
            )
        except BaseException:
            release_manager()
            raise

    def __enter__(self) -> "ManagerSession":
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.close()

    def _check_open(self) -> None:
        if self._closed:
            raise RuntimeError("Manager session is closed")

    def _result(
        self,
        key: str,
        size: int,
        raw: "NativeResult",
        outcome: str,
        *,
        model: str | None = None,
        check_elapsed_ns: int = 0,
    ) -> OperationResult:
        descriptor = raw.descriptor
        native_blobs = descriptor.blobs
        if descriptor.num_blobs != len(native_blobs):
            raise RuntimeError("Native descriptor blob count mismatch")
        blobs = tuple(
            Blob(
                blob.rank,
                blob.media,
                blob.size,
                blob.reserved,
                blob.state,
                blob.remaining_lease_ttl_ms,
                blob.gva,
            )
            for blob in native_blobs
        )
        return OperationResult(
            key,
            outcome,
            raw.code,
            raw.operation_id,
            raw.elapsed_ns,
            blobs,
            size,
            model,
            check_elapsed_ns,
        )

    def _token(
        self, result: OperationResult, kind: str, before: int
    ) -> LeaseToken:
        if len(result.blobs) != 1:
            raise RuntimeError(f"Expected one backing blob: {result}")
        blob = result.blobs[0]
        state = (
            self.native.READABLE if kind == "read" else self.native.ALLOCATED
        )
        if (
            blob.size != result.size_bytes
            or blob.reserved < blob.size
            or blob.state != state
        ):
            raise RuntimeError(f"Inconsistent native descriptor: {result}")
        return LeaseToken(
            result, kind, before + blob.remaining_lease_ttl_ms * 1000000
        )

    def begin_read(
        self, key: str, size_bytes: int, *, model: str | None = None
    ) -> tuple[OperationResult, LeaseToken | None]:
        self._check_open()
        if not key:
            raise ValueError("Cache keys must be nonempty")
        check_size(size_bytes)
        before = monotonic_ns()
        raw = self.manager.get(key)
        if raw.code == self.native.MISSING:
            outcome = "missing"
        elif raw.code == self.native.OK:
            outcome = "hit" if raw.descriptor.num_blobs else "unreadable"
        else:
            raise RuntimeError(f"Get {key!r} failed: {raw.code}")
        result = self._result(key, size_bytes, raw, outcome, model=model)
        return result, (
            self._token(result, "read", before) if outcome == "hit" else None
        )

    def begin_write(
        self, key: str, size_bytes: int, *, model: str | None = None
    ) -> tuple[OperationResult, LeaseToken | None]:
        self._check_open()
        if not key:
            raise ValueError("Cache keys must be nonempty")
        check_size(size_bytes)
        check = self.manager.check_and_evict(size_bytes)
        before = monotonic_ns()
        raw = self.manager.alloc(key, size_bytes)
        outcomes = {
            self.native.OK: "allocated",
            self.native.DUPLICATE: "duplicate",
            self.native.ALLOCATION_FAILED: "failed",
        }
        if raw.code not in outcomes:
            raise RuntimeError(
                f"Alloc {key!r} failed unexpectedly: {raw.code}"
            )
        result = self._result(
            key,
            size_bytes,
            raw,
            outcomes[raw.code],
            model=model,
            check_elapsed_ns=check.elapsed_ns,
        )
        return result, (
            self._token(result, "write", before)
            if result.outcome == "allocated"
            else None
        )

    def begin_access(self, access: Access) -> StartedAccess:
        lookup, token = self.begin_read(
            access.key, access.size_bytes, model=access.model
        )
        allocation = None
        if token is None:
            allocation, token = self.begin_write(
                access.key, access.size_bytes, model=access.model
            )
        return StartedAccess(lookup, allocation, token)

    def complete(self, token: LeaseToken) -> OperationResult:
        self._check_open()
        if monotonic_ns() >= token.deadline_ns:
            raise RuntimeError(
                "lease_validity_uncertain: native protection deadline exceeded"
            )
        result = token.result
        blob = result.blobs[0]
        action = (
            self.native.Action.WRITE_OK
            if token.kind == "write"
            else self.native.Action.READ_FINISH
        )
        raw = self.manager.update(
            result.key, result.operation_id, blob.rank, blob.media, action
        )
        if raw.code != self.native.OK:
            raise RuntimeError(f"Completion {result.key!r} failed: {raw.code}")
        return self._result(
            result.key, result.size_bytes, raw, "ok", model=result.model
        )

    def wait_for_cleanup(self) -> None:
        self._check_open()
        if not self.manager.wait_for_cleanup(self.timeout):
            raise TimeoutError("Native cleanup timed out")

    def snapshot(self, accesses: int = 0) -> Snapshot:
        self._check_open()
        self.wait_for_cleanup()
        pools = tuple(
            PoolSnapshot(
                pool["rank"],
                pool["medium"],
                pool["capacity"],
                pool["allocatedSize"],
                pool["fragmentationBytes"],
            )
            for pool in json.loads(self.manager.segment_info())
        )
        if any(
            not 0 <= pool.internal_bytes <= pool.reserved <= pool.capacity
            for pool in pools
        ):
            raise RuntimeError("Inconsistent native pool snapshot")
        return Snapshot(
            accesses, self.manager.key_count(), self.manager.reclaimed, pools
        )

    def remove(self, key: str) -> OperationResult:
        self._check_open()
        raw = self.manager.remove(key)
        if raw.code not in (self.native.OK, self.native.MISSING):
            raise RuntimeError(f"Remove {key!r} failed: {raw.code}")
        return self._result(
            key, 0, raw, "removed" if raw.code == self.native.OK else "missing"
        )

    def keys(self) -> tuple[str, ...]:
        self._check_open()
        return tuple(self.manager.keys())

    def check_and_evict(self, wanted_size_bytes: int = 0) -> None:
        self._check_open()
        self.manager.check_and_evict(wanted_size_bytes)

    def close(self) -> None:
        if not self._closed:
            try:
                self.manager.close()
            finally:
                self._closed = True
                release_manager()
