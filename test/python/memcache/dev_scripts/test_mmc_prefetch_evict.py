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

"""
Usage: python test_mmc_prefetch_evict.py

Verify the evict (DRAM -> SSD) and prefetch (SSD -> DRAM) data plane APIs.

Expected flow:
    1. put 2 keys -> DRAM blob, plus an async dual-write to SSD (2 blobs)
    2. evict -> only the upper DRAM blob is removed, SSD copy stays (checked via get_key_info)
    3. prefetch -> rewarm the SSD copy back to DRAM (checked via get_key_info)

This script does NOT call store.setup() or MetaService.setup(). All config comes
from environment variables:
    - MMC_LOCAL_CONFIG_PATH: local config (DRAM size + SSD/UBS IO), loaded by store.init()
    - MMC_META_CONFIG_PATH:  meta service config, loaded by MetaService.main()
Set them to your config files before running, e.g.:

    export MMC_LOCAL_CONFIG_PATH=/path/to/mmc-local.conf
    export MMC_META_CONFIG_PATH=/path/to/mmc-meta.conf

Blob tier is inspected with get_key_info(key, flag=0): flag=0 disables the
LayerWise auto-rewarm, so a blob is never pulled back to DRAM just by querying.

If you did not start a meta server, this script can start one itself by turning
on the START_META flag (the meta process reads MMC_META_CONFIG_PATH).
"""

import logging
import multiprocessing
import time

import acl
from memcache_hybrid import DistributedObjectStore, MetaService

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s [pyscript] %(message)s ",
)
logger = logging.getLogger(__name__)

START_META = True

BLOCK_SIZE = 1024 * 1024  # 1MB per key

# MediaType enum values exposed by type_list(): 0=HBM, 1=DRAM, 2=SSD, 3=NONE
MEDIA_NAMES = {0: "HBM", 1: "DRAM", 2: "SSD", 3: "NONE"}


def start_meta_service():
    try:
        MetaService.main()
    except Exception as e:
        logger.error("MetaService error: %s", e)


def tier_names(store, key):
    """Return human-readable media list for a key, without triggering rewarm."""
    info = store.get_key_info(key, flag=0)
    return [MEDIA_NAMES.get(t, str(t)) for t in info.type_list()]


def wait_for_tier(store, key, expected, timeout=15.0):
    """Poll until `expected` tier appears in the key's blob list (async ops)."""
    deadline = time.time() + timeout
    names = tier_names(store, key)
    while expected not in names and time.time() < deadline:
        time.sleep(0.5)
        names = tier_names(store, key)
    return names


def wait_blob_count(store, key, expected_count, timeout=10.0):
    """Poll until the key exposes at least `expected_count` blobs (async ops)."""
    deadline = time.time() + timeout
    info = store.get_key_info(key, flag=0)
    count = len(info.type_list())
    while count < expected_count and time.time() < deadline:
        time.sleep(0.5)
        info = store.get_key_info(key, flag=0)
        count = len(info.type_list())
    return count


def main():
    proc = None
    if START_META:
        proc = multiprocessing.Process(target=start_meta_service)
        proc.start()
        logger.info("MetaService started, PID: %s", proc.pid)
        time.sleep(3)

    acl.init()
    count, ret = acl.rt.get_device_count()
    logger.info("Device count: %s, get_device_count ret: %s", count, ret)
    ret = acl.rt.set_device(count - 1)
    logger.info("set_device returned: %s", ret)

    # No store.setup() here: local config (DRAM + SSD/UBS IO) is loaded from
    # the MMC_LOCAL_CONFIG_PATH environment variable inside store.init().
    store = DistributedObjectStore()
    res = store.init(count - 1)
    if res != 0:
        logger.error("init failed: %s", res)
        if proc is not None:
            _cleanup(proc)
        return
    logger.info("LocalService initialized successfully (config loaded from MMC_LOCAL_CONFIG_PATH)")

    keys = ["pf_evict_key_0001", "pf_evict_key_0002"]
    data = bytes(BLOCK_SIZE)

    try:
        # 1. put two keys -> DRAM blob + async dual-write to SSD
        for key in keys:
            res = store.put(key, data)
            if res != 0:
                logger.error("put '%s' FAILED (res=%s)", key, res)
                return
            logger.info("put '%s' OK", key)
            names = wait_for_tier(store, key, "DRAM")
            logger.info("after put, '%s' tiers: %s (expect DRAM)", key, names)
            # put triggers an async dual-write to SSD, so the key should soon
            # expose 2 blobs: DRAM + SSD.
            count = wait_blob_count(store, key, 2)
            names = tier_names(store, key)
            logger.info("after put (+wait dual-write), '%s' blobs=%d tiers: %s (expect DRAM + SSD)", key, count, names)

        # 2. evict -> only the DRAM blob is removed, SSD copy stays
        res = store.evict(keys)
        logger.info("evict returned: %s", res)
        for key in keys:
            names = wait_for_tier(store, key, "SSD")
            logger.info("after evict, '%s' tiers: %s", key, names)
            assert "SSD" in names, "expected key '%s' on SSD after evict, got %s" % (key, names)

        # 3. prefetch -> expect DRAM again (SSD -> DRAM, does not involve HBM)
        res = store.prefetch(keys)
        logger.info("prefetch returned: %s", res)
        for key in keys:
            names = wait_for_tier(store, key, "DRAM")
            logger.info("after prefetch, '%s' tiers: %s", key, names)
            assert "DRAM" in names, "expected key '%s' on DRAM after prefetch, got %s" % (key, names)

        logger.info("=" * 60)
        logger.info("PASS: evict moved keys DRAM -> SSD, prefetch moved them back SSD -> DRAM")
        logger.info("=" * 60)

    except AssertionError as e:
        logger.error("FAILED: %s", e)
    except KeyboardInterrupt:
        logger.info("Interrupted by user")
    finally:
        store.close()
        logger.info("LocalService closed")
        if proc is not None:
            _cleanup(proc)


def _cleanup(proc):
    if proc.is_alive():
        logger.info("Terminating MetaService...")
        proc.terminate()
        proc.join(timeout=3)
        if proc.is_alive():
            proc.kill()
            proc.join()
        logger.info("MetaService terminated")


if __name__ == "__main__":
    main()
