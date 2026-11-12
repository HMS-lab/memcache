# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Build only when explicitly requested, with the running interpreter."""

import fcntl
import hashlib
import importlib.util
import os
import subprocess
import sys
import sysconfig
from dataclasses import dataclass
from functools import wraps
from pathlib import Path
from threading import RLock
from types import ModuleType
from typing import TYPE_CHECKING, cast

if TYPE_CHECKING:
    from ._meta_manager_native import (
        Manager as NativeManager,
        Action as NativeAction,
    )

from typing import Callable, ParamSpec, TypeVar

SOURCE = Path(__file__).resolve().parent
TOOL = SOURCE.parents[2]
MODULE = "meta_manager_sim.cpp_build._meta_manager_native"


def _build_and_load_native() -> ModuleType:
    executable = os.path.abspath(sys.executable)
    interpreter = hashlib.sha256(executable.encode()).hexdigest()[:12]
    directory = (
        TOOL / "build" / f"{sys.implementation.cache_tag}-{interpreter}"
    )
    commands = [
        [
            "cmake",
            "-S",
            str(SOURCE),
            "-B",
            str(directory),
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DPython3_EXECUTABLE={executable}",
        ],
        ["cmake", "--build", str(directory), "--parallel", "2"],
    ]
    for command in commands:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if result.returncode:
            raise RuntimeError(f"Native build failed:\n{result.stdout}")

    suffix = sysconfig.get_config_var("EXT_SUFFIX")
    extension = (
        directory / "native" / f"{MODULE.rsplit(chr(46), 1)[-1]}{suffix}"
    )
    digest = hashlib.sha256(extension.read_bytes()).hexdigest()
    existing = sys.modules.get(MODULE)
    if existing is not None:
        if existing.__binary_digest__ != digest:
            raise RuntimeError(
                "Native code changed; start a new Python process."
            )
        return existing

    spec = importlib.util.spec_from_file_location(MODULE, extension)
    if spec is None or spec.loader is None:
        raise RuntimeError("Unable to load native extension")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    setattr(module, "__binary_digest__", digest)
    sys.modules[MODULE] = module
    return module


P = ParamSpec("P")
R = TypeVar("R")


@dataclass(frozen=True)
class NativeBackend:
    Manager: type["NativeManager"]
    Action: type["NativeAction"]
    OK: int
    MISSING: int
    DUPLICATE: int
    ALLOCATION_FAILED: int
    READABLE: int
    ALLOCATED: int
    binary_digest: str


_backend: NativeBackend | None = None


def load_native() -> NativeBackend:
    if _backend is None:
        raise RuntimeError("Call ensure_native before opening a session")
    return _backend


_build_lock = RLock()


def ensure_native() -> None:
    global _backend
    with _build_lock:
        if _manager_active:
            raise RuntimeError("Cannot build while a manager is active")
        (TOOL / "build").mkdir(exist_ok=True)
        with (TOOL / "build" / "native.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            module = _build_and_load_native()
            _backend = NativeBackend(
                module.Manager,
                module.Action,
                module.OK,
                module.MISSING,
                module.DUPLICATE,
                module.ALLOCATION_FAILED,
                module.READABLE,
                module.ALLOCATED,
                module.__binary_digest__,
            )


def build_required(function: Callable[P, R]) -> Callable[P, R]:
    @wraps(function)
    def wrapped(*args: P.args, **kwargs: P.kwargs) -> R:
        ensure_native()
        return function(*args, **kwargs)

    return wrapped


_manager_active = False


def claim_manager() -> None:
    global _manager_active
    with _build_lock:
        if _manager_active:
            raise RuntimeError("Only one native manager may be active")
        _manager_active = True


def release_manager() -> None:
    global _manager_active
    with _build_lock:
        _manager_active = False
