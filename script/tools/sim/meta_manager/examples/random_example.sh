#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

python sim_random.py --pools 2:8MiB \
                     --memory-plot build/random_example.png \
                     --detailed-stats \
                     --progress-bar \
                     --num-reads 1000 \
                     --num-blocks 200 \
                     --size-weights 4KiB:3,64KiB:1 "$@"
