#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

python -m isort sim_*.py src tests tools
python -m black sim_*.py src tests tools
python -m pycodestyle sim_*.py src tests tools

clang-format -i src/meta_manager_sim/native/src/*.cpp \
                src/meta_manager_sim/native/src/*.h
