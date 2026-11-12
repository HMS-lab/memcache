# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
from pathlib import Path

import pytest

from meta_manager_sim.config import ManagerConfig, RunConfig
from meta_manager_sim.simulation import run_simulation
from meta_manager_sim.workloads.random.random_workload import (
    RandomConfig,
    prepare_random,
)
from meta_manager_sim.workloads.trace.trace_reader import parse_line_selection
from meta_manager_sim.workloads.trace.trace_workload import (
    TraceConfig,
    block_key,
    prepare_trace,
)


def test_random_inputs_repeat_and_share_sizes() -> None:
    config = RandomConfig(accesses=100, source_count=4, key_count=3)
    first = prepare_random(config)
    second = prepare_random(config)
    assert [list(source) for source in first.sources] == [
        list(source) for source in second.sources
    ]
    assert first.info.accesses == 100


@pytest.mark.parametrize("execution", ["ordered", "concurrent"])
@pytest.mark.native
def test_real_native_counts_and_size_failures(execution: str) -> None:
    config = RandomConfig(
        accesses=100,
        key_count=12,
        source_count=4,
        size_weights=((4096, 1), (131072, 1)),
    )
    result = run_simulation(
        prepare_random(config),
        ManagerConfig(capacity_bytes=65536, lease_ttl_ms=10000),
        RunConfig(execution=execution),
    )
    metrics = result["metrics"]
    assert metrics["accesses"] == 100
    rows = {row["size_bytes"]: row for row in metrics["by_size"]}
    assert rows[131072]["allocation_failures"] > 0
    assert rows[4096]["allocation_successes"] > 0
    assert result["samples"][-1]["pools"][0]["reserved_bytes"] <= 65536


@pytest.mark.native
def test_virtual_hold_and_shared_key(tmp_path: Path) -> None:
    path = tmp_path / "trace.jsonl"
    path.write_text(
        (
            '{"hash_id_scope":"local","block_size":64,"requests":[{"'
            'type":"s","t":0,"model":"m","hash_ids":[7]}]}\n'
            '{"hash_id_scope":"local","block_size":64,"requests":[{"'
            'type":"s","t":0.005,"model":"m","hash_ids":[7]},{"type"'
            ':"s","t":0.020,"model":"m","hash_ids":[7]}]}\n'
        )
    )
    with prepare_trace(TraceConfig(path, (("m", 4096),))) as workload:
        result = run_simulation(
            workload,
            ManagerConfig(capacity_bytes=65536, lease_ttl_ms=10000),
            RunConfig(execution="concurrent", write_hold_ns=10000000),
        )
    outcomes = result["metrics"]["outcomes"]
    assert outcomes["get.unreadable"] == 1
    assert outcomes["alloc.duplicate"] == 1
    assert outcomes["get.hit"] == 1
    assert result["samples"][-1]["registered_keys"] == 1


def test_key_scope_and_selection() -> None:
    assert block_key("m", 7) == "m:7"
    assert parse_line_selection("1,2:4,3:5").intervals == ((1, 5),)


@pytest.mark.parametrize(
    "config",
    [
        {"accesses": 0},
        {"source_count": 0},
        {"size_weights": ((4096, float("nan")),)},
    ],
)
def test_invalid_random(config: dict[str, object]) -> None:
    with pytest.raises(ValueError):
        RandomConfig(**config)


@pytest.mark.native
@pytest.mark.parametrize("policy", ["lru", "sharded_lru"])
def test_pending_leases_removal_and_reallocation(policy: str) -> None:
    from meta_manager_sim.cpp_build.build import ensure_native, load_native
    from meta_manager_sim.session import ManagerSession

    ensure_native()
    config = ManagerConfig(capacity_bytes=65536, eviction_policy=policy)
    with ManagerSession(load_native(), config) as session:
        write, token = session.begin_write("k", 5000)
        assert token is not None
        assert write.blobs[0].reserved == 8192
        pending, unused = session.begin_read("k", 5000)
        assert pending.outcome == "unreadable" and unused is None
        duplicate, unused = session.begin_write("k", 5000)
        assert duplicate.outcome == "duplicate" and unused is None
        session.complete(token)
        hit, read = session.begin_read("k", 5000)
        assert hit.outcome == "hit" and read is not None
        session.complete(read)
        assert session.snapshot().pools[0].internal_bytes == 3192
        assert session.remove("k").outcome == "removed"
        assert session.snapshot().pools[0].reserved == 0
        assert session.remove("k").outcome == "missing"
        _, token = session.begin_write("k", 4096)
        assert token is not None
        session.complete(token)
        assert session.snapshot().keys == 1
    session.close()
    with pytest.raises(RuntimeError):
        session.begin_read("k", 4096)


@pytest.mark.native
def test_native_threshold_and_cleanup() -> None:
    from meta_manager_sim.cpp_build.build import ensure_native, load_native
    from meta_manager_sim.session import ManagerSession

    ensure_native()
    config = ManagerConfig(
        capacity_bytes=65536,
        high_watermark_percent=75,
        low_watermark_percent=50,
    )
    with ManagerSession(load_native(), config) as session:
        for index in range(12):
            _, token = session.begin_write(str(index), 4096)
            assert token is not None
            session.complete(token)
        session.check_and_evict()
        assert session.snapshot().keys == 12
        _, token = session.begin_write("over", 4096)
        assert token is not None
        session.complete(token)
        session.check_and_evict()
        assert session.snapshot().keys < 13


def test_trace_validation_precedes_build(tmp_path: Path) -> None:
    path = tmp_path / "bad.jsonl"
    path.write_text('{"requests": []}\n')
    with pytest.raises(ValueError):
        with prepare_trace(TraceConfig(path, (("m", 4096),))):
            pytest.fail("Invalid input was accepted")


@pytest.mark.parametrize("text", ["0", "-1", "3:1", "1,,2"])
def test_invalid_parse_line_selection(text: str) -> None:
    with pytest.raises(ValueError):
        parse_line_selection(text)


@pytest.mark.parametrize(
    "arguments", [{"workers": 0}, {"read_hold_ns": -1}, {"write_hold_ns": 1}]
)
def test_invalid_execution(arguments: dict[str, object]) -> None:
    with pytest.raises(ValueError):
        RunConfig(**arguments)


@pytest.mark.native
def test_native_fragmentation_recovery() -> None:
    from meta_manager_sim.cpp_build.build import ensure_native, load_native
    from meta_manager_sim.session import ManagerSession

    ensure_native()
    config = ManagerConfig(
        capacity_bytes=65536,
        low_watermark_percent=90,
        high_watermark_percent=100,
    )
    with ManagerSession(load_native(), config) as session:
        for index in range(16):
            _, token = session.begin_write(str(index), 4096)
            assert token is not None
            session.complete(token)
        for index in range(0, 16, 2):
            session.remove(str(index))
        before = session.snapshot()
        result, token = session.begin_write("large", 8192)
        assert result.outcome == "allocated" and token is not None
        session.complete(token)
        after = session.snapshot()
        assert after.reclaimed_blobs > before.reclaimed_blobs
        assert after.pools[0].reserved == 36864


def test_imports_and_help_do_not_build(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import runpy

    from meta_manager_sim.cpp_build import build

    def forbidden() -> None:
        raise AssertionError("Help attempted native compilation")

    monkeypatch.setattr(build, "ensure_native", forbidden)
    root = Path(__file__).resolve().parents[1]
    for name in ("sim_random.py", "sim_trace.py"):
        module = runpy.run_path(str(root / name))
        with pytest.raises(SystemExit) as error:
            module["parser"]().parse_args(["--help"])
        assert error.value.code == 0


@pytest.mark.native
def test_native_max_alignment() -> None:
    from meta_manager_sim.cpp_build.build import ensure_native, load_native
    from meta_manager_sim.session import ManagerSession

    ensure_native()
    config = ManagerConfig(capacity_bytes=65536, max_value_alignment=True)
    with ManagerSession(load_native(), config) as session:
        for key, size in (("large", 10000), ("small", 1)):
            _, token = session.begin_write(key, size)
            assert token is not None
            session.complete(token)
        snapshot = session.snapshot()
        assert snapshot.pools[0].reserved == 24576
        assert snapshot.pools[0].internal_bytes == 24576 - 10001


@pytest.mark.native
def test_access_context_survives_write_and_read_completion() -> None:
    from dataclasses import FrozenInstanceError

    from meta_manager_sim.cpp_build.build import ensure_native, load_native
    from meta_manager_sim.session import ManagerSession
    from meta_manager_sim.workloads.types import Access

    ensure_native()
    config = ManagerConfig(capacity_bytes=65536)
    access = Access("model:context", 5000, model="model")
    with ManagerSession(load_native(), config) as session:
        for expected in ("missing", "hit"):
            started = session.begin_access(access)
            assert started.lookup.outcome == expected
            assert started.lookup.model == "model"
            token = started.token
            assert token is not None
            assert token.result.model == "model"
            assert token.result.size_bytes == 5000
            if started.allocation is not None:
                assert started.allocation == token.result
                assert started.allocation.check_elapsed_ns >= 0
            else:
                assert started.lookup == token.result
            with pytest.raises(FrozenInstanceError):
                setattr(token.result, "model", "changed")
            completion = session.complete(token)
            assert completion.model == "model"
            assert completion.size_bytes == 5000
            assert completion.operation_id == token.result.operation_id
            assert completion.outcome == "ok"


@pytest.mark.native
def test_empty_trace_reports(tmp_path: Path) -> None:
    from meta_manager_sim.profiling.reporting import write_reports

    path = tmp_path / "empty.jsonl"
    path.write_text(
        '{"hash_id_scope":"local","block_size":64,"requests":[]}\n'
    )
    with prepare_trace(TraceConfig(path, (("m", 4096),))) as workload:
        result = run_simulation(workload, ManagerConfig(capacity_bytes=65536))
    assert result["metrics"]["accesses"] == 0
    assert result["metrics"]["cache_hit_rate_percent"] is None
    assert [sample["reason"] for sample in result["samples"]] == [
        "initial",
        "final",
    ]
    write_reports(result, tmp_path / "result.json", tmp_path / "csv")
    assert (tmp_path / "csv" / "sizes.csv").exists()
