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

import multiprocessing as mp
import os
import sys

import torch
import torch_npu
from mutil_process import read_worker, write_worker

if __name__ == "__main__":
    testcase = sys.argv[1]
    process_count = int(sys.argv[2])
    batch_size = int(sys.argv[3])
    block_size = int(sys.argv[4])
    call_count = int(sys.argv[5])
    warmup_count = int(sys.argv[6])
    data_dim = int(sys.argv[7])
    backend = sys.argv[8]
    local_type = sys.argv[9]

    if call_count <= 0 or warmup_count < 0:
        raise ValueError("call_count must be positive and warmup_count must not be negative")

    print(
        f"主进程 PID: {os.getpid()}, {testcase=}, {process_count=}, {batch_size=}, {block_size=}, "
        f"{call_count=}, {warmup_count=}, {data_dim=}, {backend=}, {local_type=}"
    )

    sync = mp.Barrier(process_count)
    # Base device: honors MMC_DEVICE_ID (set by run_all_examples.sh to an idle
    # NPU) so single-process smoke tests don't land on a busy device 0. Defaults
    # to 0 (the index-only behaviour) when unset, so multi-process runs still
    # fan out across consecutive devices starting from the base.
    base_device = int(os.environ.get("MMC_DEVICE_ID", 0))
    process = []
    # 创建两个子进程
    for index in range(process_count):
        if testcase == "read":
            p = mp.Process(
                target=read_worker,
                args=(
                    base_device + index,
                    batch_size,
                    block_size,
                    call_count,
                    warmup_count,
                    data_dim,
                    backend,
                    local_type,
                    process_count,
                    sync,
                ),
            )
            p.start()
            process.append(p)
        elif testcase == "write":
            p = mp.Process(
                target=write_worker,
                args=(
                    base_device + index,
                    batch_size,
                    block_size,
                    call_count,
                    warmup_count,
                    data_dim,
                    backend,
                    local_type,
                    process_count,
                    sync,
                ),
            )
            p.start()
            process.append(p)
        else:
            print(f"{testcase=} error")

    for i in range(process_count):
        process[i].join()
    print("所有子进程结束。")
