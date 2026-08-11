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

"""Verify SSD->DRAM rewarm via the get_key_info auto-copy flag.

DRAM pool = 1GB, evict threshold high = 90%. Write 11 keys of 100MB each
(1.1GB > 900MB threshold) so the first key written is evicted to SSD. Reading
the first key with the default flag (DRAM | AUTO_COPY) triggers the SSD->DRAM
rewarm path inside meta service.

Prerequisites:
  - Ascend NPU environment (torch_npu / acl).
  - MetaService flag support: get_key_info defaults to DRAM|AUTO_COPY and
    Query/PrefetchKeys perform SSD->DRAM rewarm (see mmc_meta_manager.cpp).
  - An SSD pool configured (e.g. mmc-local.conf) for evict-to-SSD + rewarm.
"""

import multiprocessing
import time
import unittest

import torch
import torch_npu

from memcache_hybrid import (
    DistributedObjectStore,
    KeyInfo,
    L2G,
    G2L,
    G2H,
    H2G,
    AUTO,
)
import acl

acl.init()
device_count, _ = acl.rt.get_device_count()
print("device count:", device_count)
acl.rt.set_device(device_count - 1)


class TestSsdRewarm(unittest.TestCase):
    """Verify SSD->DRAM rewarm."""

    BLOB_SIZE = 100 * 1024 * 1024  # 100 MB
    KEY_COUNT = 11  # 10 fill DRAM (1GB) + 1 triggers evict of the first to SSD

    @classmethod
    def setUpClass(cls):
        cls.store = DistributedObjectStore()
        ret = cls.store.init(device_count - 1)
        assert ret == 0, f"store.init failed: {ret}"

    @classmethod
    def tearDownClass(cls):
        cls.store.close()
        print("object store destroyed")

    def test_ssd_rewarm_first_key(self):
        keys = [f"ssd-rewarm-key-{i}" for i in range(self.KEY_COUNT)]

        # 1) Write the first 10 keys (10x100MB = 1GB) to fill the DRAM pool up to the
        #    90% evict threshold (900MB).
        first_keys = keys[:10]
        first_gvas = self.store.batch_alloc(first_keys, [self.BLOB_SIZE] * 10)
        for gva in first_gvas:
            self.assertNotEqual(gva, 0)
        src_tensors = [
            torch.full(size=(self.BLOB_SIZE,), fill_value=i % 256, dtype=torch.uint8, device="npu") for i in range(10)
        ]
        torch.npu.current_stream().synchronize()
        self.assertEqual(
            self.store.batch_copy(
                gva_ptrs=first_gvas,
                buffer_ptrs=[t.data_ptr() for t in src_tensors],
                sizes=[self.BLOB_SIZE] * 10,
                direct=L2G,
            ),
            0,
        )
        self.assertEqual(
            self.store.batch_write_finish(keys=first_keys, res=[0] * 10),
            [0] * 10,
        )

        # 2) Write the 11th key; DRAM is full -> the first key (LRU) is evicted to SSD.
        last_key = keys[10]
        last_gva = self.store.batch_alloc([last_key], [self.BLOB_SIZE])
        self.assertNotEqual(last_gva[0], 0)
        last_tensor = torch.full(size=(self.BLOB_SIZE,), fill_value=10 % 256, dtype=torch.uint8, device="npu")
        torch.npu.current_stream().synchronize()
        self.assertEqual(
            self.store.batch_copy(
                gva_ptrs=last_gva,
                buffer_ptrs=[last_tensor.data_ptr()],
                sizes=[self.BLOB_SIZE],
                direct=L2G,
            ),
            0,
        )
        self.assertEqual(self.store.batch_write_finish(keys=[last_key], res=[0]), [0])

        # 3) Read the first key (evicted to SSD) with the default flag = DRAM|AUTO_COPY,
        #    which triggers SSD->DRAM rewarm inside meta service.

        time.sleep(5)

        key_info = self.store.get_key_info(first_keys[0])
        print(f"first get_key_info of first key: {key_info}")

        # 4) Read back data and verify it matches (proves rewarm restored a DRAM blob).
        self.assertEqual(self.store.batch_add_lease(keys=[first_keys[0]]), [0])
        dst = torch.zeros(size=(self.BLOB_SIZE,), dtype=torch.uint8, device="npu")
        torch.npu.current_stream().synchronize()
        read_ret = self.store.batch_copy(
            gva_ptrs=key_info.gva_list(),
            buffer_ptrs=[dst.data_ptr()],
            sizes=[self.BLOB_SIZE],
            direct=G2L,
        )
        self.assertEqual(read_ret, 0, f"batch_copy read failed: {read_ret}")
        self.assertTrue(torch.equal(src_tensors[0], dst), "data mismatch after SSD->DRAM rewarm")
        self.assertEqual(self.store.batch_remove_lease(keys=[first_keys[0]]), 0)

        time.sleep(5)

        key_info = self.store.get_key_info(first_keys[0])
        print(f"second get_key_inf of first key: {key_info}")

        # 4) Read back data and verify it matches (proves rewarm restored a DRAM blob).
        self.assertEqual(self.store.batch_add_lease(keys=[first_keys[0]]), [0])
        dst = torch.zeros(size=(self.BLOB_SIZE,), dtype=torch.uint8, device="npu")
        torch.npu.current_stream().synchronize()
        read_ret = self.store.batch_copy(
            gva_ptrs=key_info.gva_list(),
            buffer_ptrs=[dst.data_ptr()],
            sizes=[self.BLOB_SIZE],
            direct=G2L,
        )
        self.assertEqual(read_ret, 0, f"batch_copy read failed: {read_ret}")
        self.assertTrue(torch.equal(src_tensors[0], dst), "data mismatch after SSD->DRAM rewarm")
        self.assertEqual(self.store.batch_remove_lease(keys=[first_keys[0]]), 0)

        # Cleanup.
        self.store.remove_batch(keys)


if __name__ == "__main__":
    unittest.main()
