# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Run the bundled Python examples with the real standalone simulator."""

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[1]


@pytest.mark.native
@pytest.mark.parametrize(
    "script, accesses", [("random_example.py", 200), ("trace_example.py", 17)]
)
def test_python_example(tmp_path: Path, script: str, accesses: int) -> None:
    result = subprocess.run(
        [sys.executable, str(TOOL / "examples" / script)],
        cwd=tmp_path,
        env={**os.environ, "PYTHONPATH": str(TOOL / "src")},
        capture_output=True,
        text=True,
        check=True,
    )
    assert re.search(
        rf"^Accesses\s+{accesses}\s*$", result.stdout, re.MULTILINE
    )
    assert "Allocations by requested block size" in result.stdout

    if script == "trace_example.py":
        expected = {
            "Cache hits": 10,
            "Successful allocations": 7,
            "Failed allocations": 0,
            "Registered keys": 7,
        }
        for label, count in expected.items():
            assert re.search(
                rf"^{label}\s+{count}\s*$", result.stdout, re.MULTILINE
            )
