# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from matplotlib.image import imread

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.native
@pytest.mark.parametrize(
    "command,reads,hits",
    [
        (
            (
                "bash",
                ROOT / "examples/random_example.sh",
                "--num-reads",
                "8",
                "--num-blocks",
                "1",
            ),
            8,
            7,
        ),
        (
            (
                "bash",
                ROOT / "examples/sequential_example.sh",
                "--num-reads",
                "3",
                "--block-size",
                "4KiB",
                "--pools",
                "1:64KiB",
            ),
            3,
            0,
        ),
        (("bash", ROOT / "examples/trace_example.sh"), 12, 7),
        (
            (
                sys.executable,
                ROOT / "sim_trace.py",
                "--trace-path",
                ROOT / "examples/traces.jsonl",
                "--line",
                "1",
                "--model-block-size",
                "example-model-b:8KiB",
                "--pools",
                "1:64KiB",
            ),
            5,
            2,
        ),
    ],
    ids=[
        "random-example",
        "sequential-example",
        "trace-example",
        "trace-line",
    ],
)
def test_examples_and_trace_cli(
    command: tuple[str | Path, ...], reads: int, hits: int, tmp_path: Path
) -> None:
    if (
        command[1] == ROOT / "examples/trace_example.sh"
        and shutil.which("heaptrack") is None
    ):
        pytest.skip("The trace example enables optional Heaptrack")
    path = tmp_path / "memory.png"
    result = subprocess.run(
        [*command, "--memory-plot", str(path)],
        cwd=tmp_path,
        env={
            **os.environ,
            "PATH": str(Path(sys.executable).parent)
            + os.pathsep
            + os.environ["PATH"],
            "MPLBACKEND": "Agg",
        },
        capture_output=True,
        text=True,
        timeout=180,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    for label, value in (("reads", reads), ("hits", hits)):
        assert re.search(rf"^{label}\s+{value}$", result.stdout, re.MULTILINE)
    assert imread(path).std() > 0


@pytest.mark.parametrize(
    "entry", ["sim_random.py", "sim_sequential.py", "sim_trace.py"]
)
def test_help_without_build_tools(entry: str, tmp_path: Path) -> None:
    result = subprocess.run(
        [sys.executable, str(ROOT / entry), "--help"],
        cwd=tmp_path,
        env={**os.environ, "PATH": ""},
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stderr
    assert "--pools" in result.stdout and "--memory-plot" in result.stdout
