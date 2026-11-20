# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import argparse
import re
import signal
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]

PEAK = re.compile(r"^(\d+(?:\.\d+)?)([BKMGT]) peak memory consumed over")
SOURCE = re.compile(r"^\s+at (.+:\d+)$")
UNITS = {unit: 1024**i for i, unit in enumerate("BKMGT")}


def _source_totals(report: str) -> dict[str, float]:
    totals: dict[str, float] = {}

    # Note: Both loops share an iterator
    # Skip earlier sections until the peak-memory section begins
    lines = iter(report.splitlines())
    for line in lines:
        if line == "PEAK MEMORY CONSUMERS":
            break

    size: float | None = None
    source: str | None = None

    for line in lines:
        # Stop when another section begins
        if line.startswith(
            ("MOST TEMPORARY ALLOCATIONS", "MEMORY LEAKS", "total runtime:")
        ):
            break

        if match := PEAK.match(line):
            # Add the previous entry if it had a matching source
            if size is not None and source is not None:
                totals[source] = totals.get(source, 0) + size

            size = float(match[1]) * UNITS[match[2]]
            source = None
        elif match := SOURCE.match(line):
            # Extract the source path and line number from the stack frame
            frame = match[1]

            if (
                str(ROOT) not in frame
                or "/.venv/" in frame
                or "/venv/" in frame
                or "/site-packages/" in frame
            ):
                continue

            # Note: Prefer a caller over the generic mmc_ref.h wrapper
            # Keep the first wrapper frame as a fallback until a caller appears
            if source is None or (
                "/mmc_ref.h:" in source and "/mmc_ref.h:" not in frame
            ):
                source = frame

    # Note: The loop adds each entry when the next one begins
    # Add the last entry, since the loop hasn't added it yet
    if size is not None and source is not None:
        totals[source] = totals.get(source, 0) + size

    return totals


def main() -> None:
    # Exit without a traceback when a pipe consumer stops reading
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()

    report = subprocess.run(
        [
            "heaptrack_print",
            args.capture,
            "--merge-backtraces=0",
            "--print-allocators=0",
            "--print-temporary=0",
            "--peak-limit=1000000",
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout

    # Group peak memory by source and print the largest totals first
    for source, size in sorted(
        _source_totals(report).items(), key=lambda item: item[1], reverse=True
    ):
        print(f"{size / 1024:10.2f} KiB {source}")


if __name__ == "__main__":
    main()
