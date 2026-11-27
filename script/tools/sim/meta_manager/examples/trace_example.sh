#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

python sim_trace.py --pools 1:64KiB \
                    --memory-plot build/trace_example.png \
                    --detailed-stats \
                    --profile \
                    --progress-bar \
                    --trace-path examples/traces.jsonl \
                    --line 0 \
                    --model-block-size example-model-a:4KiB \
                    --model-block-size example-model-b:8KiB "$@"
