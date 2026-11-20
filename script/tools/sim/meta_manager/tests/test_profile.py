# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from pytest import MonkeyPatch

import meta_manager_sim.simulation as simulation
from meta_manager_sim.config import RunConfig
from meta_manager_sim.metrics import read_rss
from meta_manager_sim.simulation import _simulate_reads

pytestmark = pytest.mark.native
ROOT = Path(__file__).resolve().parents[1]


def test_heaptrack_profiles_the_native_worker(
    monkeypatch: MonkeyPatch,
) -> None:
    if shutil.which("heaptrack") is None:
        pytest.skip("Optional Heaptrack not on PATH")
    executables: list[Path] = []

    def observe(pid: int) -> int:
        executables.append(Path(f"/proc/{pid}/exe").resolve())
        return read_rss(pid)

    monkeypatch.setattr(simulation, "read_rss", observe)
    metrics = _simulate_reads(
        [("profile-key", 4096), ("profile-key", 4096)], RunConfig(profile=True)
    )
    assert metrics["summary"].at[0, "hits"] == 1
    assert (metrics["memory"]["rss_bytes"] > 0).all()
    capture = Path(str(metrics["summary"].at[0, "profile_path"]))
    assert capture.stat().st_size > 0
    retained = capture.parent / "native_worker"
    assert retained.is_file()
    assert executables == [retained, retained]
    if printer := shutil.which("heaptrack_print"):
        analysis = subprocess.run(
            [printer, str(capture)],
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
        )
        assert "MmcMetaManager" in analysis.stdout
        sources = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/heap_sources.py"),
                str(capture),
            ],
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
        )
        rows = [line.split(maxsplit=2) for line in sources.stdout.splitlines()]
        assert rows and all(unit == "KiB" for _, unit, _ in rows)
        assert all(
            source.startswith(str(ROOT.parents[3])) for _, _, source in rows
        )
