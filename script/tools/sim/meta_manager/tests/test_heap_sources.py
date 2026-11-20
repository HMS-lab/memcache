# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import sys
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from subprocess import CompletedProcess
from unittest.mock import Mock

import pytest
from pytest import CaptureFixture, MonkeyPatch

# Note: Load by file path so both pytest and python -m pytest work
spec = spec_from_file_location(
    "heap_sources",
    Path(__file__).resolve().parents[1] / "tools/heap_sources.py",
)
assert spec is not None and spec.loader is not None
heap_sources = module_from_spec(spec)
spec.loader.exec_module(heap_sources)

CALLER = f"{heap_sources.ROOT}/src/memcache/csrc/meta_service/manager.cpp:10"
WRAPPER = f"{heap_sources.ROOT}/src/memcache/csrc/common/mmc_ref.h:20"
REPORT = f"""Earlier report section
1T peak memory consumed over an earlier section
    at {CALLER}
PEAK MEMORY CONSUMERS
1K peak memory consumed over one allocation
    at /external/library.cpp:1
    at {heap_sources.ROOT}/.venv/include/dependency.hpp:1
    at {heap_sources.ROOT}/venv/include/dependency.hpp:2
    at {heap_sources.ROOT}/site-packages/dependency.hpp:3
    at {WRAPPER}
    at {CALLER}
    at {heap_sources.ROOT}/src/another_caller.cpp:30
512B peak memory consumed over another allocation
    at {CALLER}
8G peak memory consumed over an external allocation
    at /external/library.cpp:1
2M peak memory consumed over a wrapper allocation
    at {WRAPPER}
    at {heap_sources.ROOT}/src/memcache/csrc/common/mmc_ref.h:40
"""


@pytest.mark.parametrize("footer", ["", "MEMORY LEAKS"])
def test_peak_sizes_are_grouped_by_project_source(footer: str) -> None:
    report = REPORT
    if footer:
        report += (
            f"{footer}\n"
            "4T peak memory consumed over a later section\n"
            f"    at {CALLER}\n"
        )
    assert heap_sources._source_totals(report) == {
        CALLER: 1536,
        WRAPPER: 2 * 1024**2,
    }


def test_cli_passes_the_capture_and_prints_largest_sources_first(
    tmp_path: Path, monkeypatch: MonkeyPatch, capsys: CaptureFixture[str]
) -> None:
    capture = tmp_path / "capture.zst"

    def run_printer(
        command: list[str | Path], **options: object
    ) -> CompletedProcess[str]:
        assert command == [
            "heaptrack_print",
            capture,
            "--merge-backtraces=0",
            "--print-allocators=0",
            "--print-temporary=0",
            "--peak-limit=1000000",
        ]
        assert options == {"check": True, "capture_output": True, "text": True}
        return CompletedProcess(command, 0, stdout=REPORT)

    monkeypatch.setattr(sys, "argv", ["heap_sources.py", str(capture)])
    monkeypatch.setattr(heap_sources.subprocess, "run", run_printer)
    monkeypatch.setattr(heap_sources.signal, "signal", Mock())
    heap_sources.main()

    rows = [
        line.split(maxsplit=2) for line in capsys.readouterr().out.splitlines()
    ]
    assert rows == [["2048.00", "KiB", WRAPPER], ["1.50", "KiB", CALLER]]
