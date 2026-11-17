#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

# Sourced by build.sh. Keep interpreter, development artifacts and packaging aligned.
if [ -n "${PYTHON_EXECUTABLE:-}" ]; then
    MMC_SELECTED_PYTHON="$PYTHON_EXECUTABLE"
elif [ -n "${VIRTUAL_ENV:-}" ]; then
    MMC_SELECTED_PYTHON="$VIRTUAL_ENV/bin/python"
elif [ -n "${PYTHON_HOME:-}" ]; then
    MMC_SELECTED_PYTHON="$PYTHON_HOME/bin/python3"
elif [ -x /usr/local/python3.11/bin/python3 ]; then
    MMC_SELECTED_PYTHON=/usr/local/python3.11/bin/python3
else
    MMC_SELECTED_PYTHON=python3
fi

if ! MMC_SELECTED_PYTHON=$(command -v "$MMC_SELECTED_PYTHON"); then
    echo "Cannot find the selected Python. Use --python /path/to/python3.11." >&2
    return 1
fi

if ! MMC_PYTHON_METADATA=$("$MMC_SELECTED_PYTHON" - <<'PYTHON'
import os
import sys
import sysconfig

if sys.version_info < (3, 8):
    raise SystemExit("MemCache requires Python 3.8 or later")
include = sysconfig.get_path("include")
libdir = sysconfig.get_config_var("LIBDIR") or os.path.join(sys.base_prefix, "lib")
library = os.path.join(libdir, sysconfig.get_config_var("LDLIBRARY") or "")
if not os.path.isfile(os.path.join(include, "Python.h")) or not os.path.isfile(library):
    raise SystemExit(
        f"Install development headers and the shared library for {sys.executable}: "
        f"expected {include}/Python.h and {library}"
    )
# Preserve the venv path; resolving interpreter symlinks would lose package isolation.
for value in (os.path.abspath(sys.executable), sys.prefix, include, library, libdir):
    print(value)
PYTHON
); then
    return 1
fi
mapfile -t MMC_PYTHON_PATHS <<< "$MMC_PYTHON_METADATA"
export PYTHON_EXECUTABLE="${MMC_PYTHON_PATHS[0]}"
export PYTHON_HOME="${MMC_PYTHON_PATHS[1]}"
MMC_PYTHON_INCLUDE_DIR="${MMC_PYTHON_PATHS[2]}"
MMC_PYTHON_LIBRARY="${MMC_PYTHON_PATHS[3]}"
export PATH="$(dirname "$PYTHON_EXECUTABLE"):$PATH"
export LD_LIBRARY_PATH="${MMC_PYTHON_PATHS[4]}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CMAKE_PREFIX_PATH="${PYTHON_HOME}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
echo "Python interpreter: $PYTHON_EXECUTABLE"
echo "Python headers:     $MMC_PYTHON_INCLUDE_DIR"
echo "Python library:     $MMC_PYTHON_LIBRARY"
