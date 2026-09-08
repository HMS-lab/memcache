#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import ctypes
from enum import IntEnum
import os
import sys


class _BmCopyType(IntEnum):
    L2G = 0
    G2L = 1
    G2H = 2
    H2G = 3
    AUTO = 9


L2G = _BmCopyType.L2G  # 拷贝方向：本地 HBM → 全局空间
G2L = _BmCopyType.G2L  # 拷贝方向：全局空间 → 本地 HBM
G2H = _BmCopyType.G2H  # 拷贝方向：全局空间 → 本地 Host DRAM
H2G = _BmCopyType.H2G  # 拷贝方向：本地 Host DRAM → 全局空间
AUTO = _BmCopyType.AUTO  # 拷贝方向：自动推断方向

__all__ = [
    "DistributedObjectStore",
    "KeyInfo",
    "LocalConfig",
    "MetaConfig",
    "MetaService",
    "ReplicateConfig",
    "L2G",
    "G2L",
    "G2H",
    "H2G",
    "AUTO",
]

current_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.append(current_dir)

lib_dir = os.path.join(current_dir, "lib")
# Preload with absolute paths so that subsequent dlopen can resolve without LD_LIBRARY_PATH.
_preload_list = [
    "libmf_memcache.so",
    "libubsio_kvc.so.1",
]
for lib_name in _preload_list:
    so_path = os.path.join(lib_dir, lib_name)
    if os.path.exists(so_path):
        ctypes.CDLL(so_path)

from _pymmc import (
    DistributedObjectStore,
    KeyInfo,
    LocalConfig,
    MetaConfig,
    MetaService,
    ReplicateConfig,
)
