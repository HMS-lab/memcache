#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

python sim_sequential.py --pools 1:10GiB,2:4GiB \
                         --memory-plot build/sequential_example.png \
                         --detailed-stats \
                         --num-reads 1000000 \
                         --block-size 64MiB "$@"
