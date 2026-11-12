#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
python=${PYTHON:-python3}
"$python" -m isort sim_trace.py sim_random.py src tests
# One file per invocation avoids multiprocessing worker shutdown in sandboxes.
rg --files -0 -g '*.py' -g '*.pyi' sim_trace.py sim_random.py src tests |
    xargs -0 -r -n 1 "$python" -m black
"$python" -m pycodestyle sim_trace.py sim_random.py src tests
