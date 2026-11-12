# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Coordinate access streams and finite virtual-time cohorts."""

import heapq
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, replace
from time import monotonic_ns, perf_counter

from meta_manager_sim.config import ManagerConfig, RunConfig
from meta_manager_sim.cpp_build.build import build_required, load_native
from meta_manager_sim.profiling.metrics import Metrics, snapshot_metrics
from meta_manager_sim.profiling.models import (
    LeaseToken,
    OperationResult,
    SimulationResult,
    Snapshot,
    StartedAccess,
)
from meta_manager_sim.session import ManagerSession
from meta_manager_sim.workloads.types import Access, Workload

MAX_TIME_NS = (1 << 64) - 1


class _Coordinator:
    """One owner for cursors, metrics, held leases, and virtual time."""

    def __init__(
        self, workload: Workload, run: RunConfig, session: ManagerSession
    ) -> None:
        self.workload = workload
        self.config = run
        self.session = session
        self.metrics = Metrics()
        self.ready: list[tuple[int, int, Access]] = []
        self.pending: list[tuple[int, int, Access, LeaseToken]] = []
        self.owned: dict[int, LeaseToken] = {}
        self.previous: dict[int, int] = {}
        self.delayed_accesses = 0
        self.queue_delay_ns = 0
        self.max_queue_delay_ns = 0
        self.max_active_operations = 0
        self.completed = 0
        self.now = 0
        self.next_sample = run.sample_every
        self.samples: list[Snapshot] = []

    def _advance(self, source: int) -> None:
        access = next(self.workload.sources[source], None)
        if access is None:
            return
        if not self.previous.get(source, 0) <= access.at_ns <= MAX_TIME_NS:
            raise ValueError(
                "Source timestamp is nonmonotonic or out of range"
            )
        self.previous[source] = access.at_ns
        heapq.heappush(
            self.ready, (max(self.now, access.at_ns), source, access)
        )

    def _check_leases(self) -> None:
        if self.owned and monotonic_ns() >= min(
            token.deadline_ns for token in self.owned.values()
        ):
            raise RuntimeError("lease_validity_uncertain")

    def _record_start(self, result: StartedAccess, access: Access) -> None:
        if result.token is not None:
            self.owned[result.token.result.operation_id] = result.token
        delay = self.now - access.at_ns
        self.delayed_accesses += delay > 0
        self.queue_delay_ns += delay
        self.max_queue_delay_ns = max(self.max_queue_delay_ns, delay)
        self.max_active_operations = max(
            self.max_active_operations, len(self.owned)
        )
        self.metrics.record("get", result.lookup)
        if result.allocation is not None:
            self.metrics.record("alloc", result.allocation)

    def _finish(
        self,
        source: int,
        token: LeaseToken | None,
        completion: OperationResult | None = None,
    ) -> None:
        if token is not None:
            assert completion is not None
            self.metrics.record(f"finish_{token.kind}", completion)
            del self.owned[token.result.operation_id]
        self.completed += 1
        self._advance(source)

    def _sample_if_due(self) -> None:
        if self.next_sample and self.completed >= self.next_sample:
            self._check_leases()
            self.samples.append(
                replace(
                    self.session.snapshot(self.completed),
                    virtual_time_ns=self.now,
                    requested_threshold=self.next_sample,
                )
            )
            self._check_leases()
            self.next_sample = (
                self.completed // self.config.sample_every + 1
            ) * self.config.sample_every

    def _ordered(self) -> None:
        while self.ready:
            self.now, source, access = heapq.heappop(self.ready)
            result = self.session.begin_access(access)
            self._record_start(result, access)
            completion = (
                self.session.complete(result.token) if result.token else None
            )
            self._finish(source, result.token, completion)
            self._sample_if_due()

    def _start_round(self, pool: ThreadPoolExecutor) -> None:
        starts = []
        while self.ready and self.ready[0][0] == self.now:
            _, source, access = heapq.heappop(self.ready)
            starts.append(
                (
                    source,
                    access,
                    pool.submit(self.session.begin_access, access),
                )
            )

        errors: list[BaseException] = []
        for source, access, future in starts:
            try:
                result = future.result()
                self._record_start(result, access)
                token = result.token
                if token is None:
                    self._finish(source, None)
                    continue
                hold = (
                    self.config.read_hold_ns
                    if token.kind == "read"
                    else self.config.write_hold_ns
                )
                if self.now + hold > MAX_TIME_NS:
                    raise ValueError("Virtual completion timestamp overflow")
                heapq.heappush(
                    self.pending, (self.now + hold, source, access, token)
                )
            except BaseException as error:
                errors.append(error)
        if errors:
            raise errors[0]

    def _finish_round(self, pool: ThreadPoolExecutor) -> None:
        finishes = []
        while self.pending and self.pending[0][0] == self.now:
            _, source, _, token = heapq.heappop(self.pending)
            finishes.append(
                (source, token, pool.submit(self.session.complete, token))
            )

        errors: list[BaseException] = []
        for source, token, future in finishes:
            try:
                self._finish(source, token, future.result())
            except BaseException as error:
                errors.append(error)
        if errors:
            raise errors[0]

    def _concurrent(self) -> None:
        with ThreadPoolExecutor(max_workers=self.config.workers) as pool:
            while self.ready or self.pending:
                self.now = min(
                    self.ready[0][0] if self.ready else MAX_TIME_NS + 1,
                    self.pending[0][0] if self.pending else MAX_TIME_NS + 1,
                )
                self._check_leases()
                if self.pending and self.pending[0][0] == self.now:
                    self._finish_round(pool)
                else:
                    self._start_round(pool)
                self._sample_if_due()

    def run(self) -> None:
        try:
            for source in range(len(self.workload.sources)):
                self._advance(source)
            if self.config.execution == "ordered":
                self._ordered()
            else:
                self._concurrent()
            if self.completed != self.workload.info.accesses:
                raise RuntimeError(
                    "Completed count differs from prepared input"
                )
            self.metrics.validate()
        finally:
            # All submitted futures have been joined before releasing tokens.
            for token in self.owned.values():
                try:
                    self.session.complete(token)
                except (RuntimeError, TimeoutError):
                    pass


@build_required
def run_simulation(
    workload: Workload, manager: ManagerConfig, run: RunConfig = RunConfig()
) -> SimulationResult:
    backend = load_native()
    with ManagerSession(backend, manager) as session:
        coordinator = _Coordinator(workload, run, session)
        initial = replace(session.snapshot(), reason="initial")
        started = perf_counter()
        coordinator.run()
        final = replace(
            session.snapshot(coordinator.completed),
            reason="final",
            virtual_time_ns=coordinator.now,
        )
        elapsed = perf_counter() - started
        samples = [initial, *coordinator.samples, final]
        return {
            "schema_version": 3,
            "status": "completed",
            "workload": asdict(workload.info),
            "configuration": {
                **asdict(manager),
                **asdict(run),
                "workers": run.workers if run.execution == "concurrent" else 1,
            },
            "provenance": {
                "native_mutation_safety": "unresolved",
                "extension_sha256": backend.binary_digest,
                "time_model": "cohort; native lease clock remains real",
            },
            "metrics": {
                **coordinator.metrics.summary(),
                "elapsed_wall_seconds": elapsed,
                "accesses_per_second": (
                    coordinator.completed / elapsed if elapsed else None
                ),
                "virtual_duration_ns": coordinator.now,
                "max_active_native_calls": session.manager.peak_calls,
                "max_active_operations": coordinator.max_active_operations,
                "delayed_accesses": coordinator.delayed_accesses,
                "queue_delay_ns": coordinator.queue_delay_ns,
                "max_queue_delay_ns": coordinator.max_queue_delay_ns,
            },
            "samples": [snapshot_metrics(sample) for sample in samples],
        }
