# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import json
import os
import shutil
import signal
import subprocess
import tempfile
from contextlib import suppress
from pathlib import Path
from typing import TextIO, cast

from .build import ROOT, build_native


class NativeProcess:
    def __init__(self, profile: bool) -> None:
        profiler = shutil.which("heaptrack") if profile else None
        if profile and profiler is None:
            raise RuntimeError("--profile requires heaptrack on PATH")

        self._profile_dir: Path | None = None
        command = [build_native()]

        if profiler:
            # Create a unique output directory for this run
            directory = ROOT / "build" / "profiles"
            directory.mkdir(parents=True, exist_ok=True)
            self._profile_dir = Path(
                tempfile.mkdtemp(prefix="heaptrack-", dir=directory)
            )

            # Keep the executable alongside its allocation capture
            retained = self._profile_dir / "native_worker"
            shutil.copy2(command[0], retained)

            command = [
                profiler,
                "--output",
                self._profile_dir / "capture",
                retained,
            ]

        self._subprocess = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        self._stdin = cast(TextIO, self._subprocess.stdin)
        self._stdout = cast(TextIO, self._subprocess.stdout)

    def __enter__(self) -> "NativeProcess":
        return self

    def __exit__(self, *_) -> None:
        self.cleanup()

    def request(self, operation: str, **kwargs: object) -> dict[str, object]:
        self._stdin.write(json.dumps({"op": operation, **kwargs}) + "\n")
        self._stdin.flush()

        # Note: Heaptrack writes status messages to the worker's stdout pipe
        for line in self._stdout:
            if self._profile_dir is None or line.startswith("{"):
                break
        else:
            raise RuntimeError(
                f"Native worker exited unexpectedly: {self._subprocess.poll()}"
            )

        reply = json.loads(line)
        if not reply["ok"]:
            raise RuntimeError(f"Native worker failed: {reply}")

        return reply["result"]

    def close(self) -> Path | None:
        self.request("stop")
        self._subprocess.wait(timeout=30)

        if self._profile_dir is None:
            return None

        # Find the capture using the previously passed prefix
        captures: list[Path] = []
        for filepath in self._profile_dir.glob("capture*"):
            if filepath.is_file() and filepath.stat().st_size > 0:
                captures.append(filepath)

        if len(captures) != 1:
            raise RuntimeError(
                f"Expected one Heaptrack capture, found {len(captures)}"
            )

        return captures[0]

    def cleanup(self) -> None:
        # Note: Closing stdin tells the worker there are no more requests,
        # allowing it to exit and Heaptrack to finish saving its capture
        with suppress(BrokenPipeError):
            self._stdin.close()

        try:
            self._subprocess.wait(timeout=5)
        except subprocess.TimeoutExpired:
            # Note: The process group may exit before the signal is sent
            with suppress(ProcessLookupError):
                os.killpg(self._subprocess.pid, signal.SIGKILL)
            self._subprocess.wait()

        self._stdout.close()
