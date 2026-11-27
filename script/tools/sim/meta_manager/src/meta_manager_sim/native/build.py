# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import fcntl
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

""" ----------------------------------------------------------------------- """


def _subprocess_run(*args) -> None:
    result = subprocess.run([*args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def _cmake_configure(build_dir: Path) -> None:
    _subprocess_run(
        "cmake",
        "-S",
        Path(__file__).parent,
        "-B",
        build_dir,
        "-DCMAKE_BUILD_TYPE=Release",
    )


def _cmake_build(build_dir: Path) -> None:
    _subprocess_run("cmake", "--build", build_dir)


""" ----------------------------------------------------------------------- """


def build_native() -> Path:
    build_dir = ROOT / "build" / "native"
    build_dir.mkdir(parents=True, exist_ok=True)

    with (build_dir / "build.lock").open("w") as lock:
        # Prevent concurrent builds in the same directory
        fcntl.flock(lock, fcntl.LOCK_EX)

        _cmake_configure(build_dir)
        _cmake_build(build_dir)

    # Return the compiled worker's executable path
    return build_dir / "bin" / "meta_manager_sim_native"
