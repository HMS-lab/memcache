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
Usage: python test_mmc_eviction.py

This script quickly create small local DRAM pool, insert keys until eviction happens. There will be exactly
6 keys get evicted.

If you did not start meta server, this script can start meta server itself, by turning on START_META flag.

This script uses default port: META_SERVICE_PORT, CONFIG_STORE_PORT, METRICS_PORT. If you
use alternative ports, please make change accordingly.
"""

import logging
import multiprocessing
import sys
import time

import acl
from memcache_hybrid import DistributedObjectStore, LocalConfig, MetaConfig, MetaService

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
logger = logging.getLogger(__name__)

START_META = True

META_HOST = "127.0.0.1"
META_SERVICE_PORT = 5088
CONFIG_STORE_PORT = 6088
METRICS_PORT = 8088
KV_EVENTS_PORT = 5557

# ZMQ CURVE server keypair (server-only auth). Subscribers need the public key.
ZMQ_PUBLIC_KEY = ""
ZMQ_SECRET_KEY = ""

# The following specs should make 6 evictions.
# Please do not make change to following parameters, or eviction may not happen.
DRAM_SIZE = "64MB"
HBM_SIZE = "0MB"
EVICTION_HIGH_PCT = 80
EVICTION_LOW_PCT = 60
BLOCK_SIZE = 1024 * 1024  # 1MB per key
NUM_BLOCKS_BEFORE_EVICT = 100


def start_meta_service():
    config = MetaConfig()
    config.meta_service_url = f"tcp://{META_HOST}:{META_SERVICE_PORT}"
    config.config_store_url = f"tcp://{META_HOST}:{CONFIG_STORE_PORT}"
    config.metrics_url = f"http://{META_HOST}:{METRICS_PORT}"
    config.evict_threshold_high = EVICTION_HIGH_PCT
    config.evict_threshold_low = EVICTION_LOW_PCT
    config.log_level = "debug"
    # kv_events broadcast + ZMQ CURVE encryption
    config.kv_events_enable = True
    config.kv_events_transport_type = 0  # KV_EVENT_TRANSPORT::ZMQ
    config.kv_events_endpoint = f"tcp://{META_HOST}:{KV_EVENTS_PORT}"
    config.kv_events_model_name = "eviction-test"
    config.kv_events_block_size = BLOCK_SIZE
    config.zmq_enable_curve = False
    config.zmq_public_key = ZMQ_PUBLIC_KEY
    config.zmq_private_key = ZMQ_SECRET_KEY
    try:
        assert MetaService.setup(config) == 0, "setup meta config failed"
        MetaService.main()
    except Exception as e:
        logger.error("MetaService error: %s", e)


def main():
    if START_META:
        proc = multiprocessing.Process(target=start_meta_service)
        proc.start()
        logger.info("MetaService started, PID: %s", proc.pid)
        time.sleep(3)

    acl.init()
    count, ret = acl.rt.get_device_count()
    logger.info("Device count: %s", count)
    ret = acl.rt.set_device(count - 1)
    logger.info("set_device returned: %s", ret)

    config = LocalConfig()
    config.meta_service_url = f"tcp://{META_HOST}:{META_SERVICE_PORT}"
    config.config_store_url = f"tcp://{META_HOST}:{CONFIG_STORE_PORT}"
    config.backend_id = "192.168.88.88"
    config.protocol = "host_tcp"
    config.dram_size = DRAM_SIZE
    config.hbm_size = HBM_SIZE
    config.max_dram_size = DRAM_SIZE
    config.max_hbm_size = HBM_SIZE
    logger.info("LocalConfig: dram=%s, hbm=%s, backend_id=%s", DRAM_SIZE, HBM_SIZE, config.backend_id)

    store = DistributedObjectStore()
    res = store.setup(config)
    if res != 0:
        logger.error("setup failed: %s", res)
        _cleanup(proc)
        return
    res = store.init(count - 1)
    if res != 0:
        logger.error("init failed: %s", res)
        _cleanup(proc)
        return
    logger.info("LocalService initialized successfully")

    data = bytes(BLOCK_SIZE)
    put_count = 0
    evicted_keys = set()
    existing_keys = set()

    logger.info("Writing %dMB blocks until eviction triggers...", BLOCK_SIZE // (1024 * 1024))
    logger.info("Eviction thresholds: high=%d%%, low=%d%%", EVICTION_HIGH_PCT, EVICTION_LOW_PCT)

    try:
        for i in range(NUM_BLOCKS_BEFORE_EVICT):
            key = f"kv_block_{i:04d}"
            res = store.put(key, data)
            if res != 0:
                logger.error("[%d] put '%s' FAILED (res=%s)", i, key, res)
                break
            put_count += 1
            existing_keys.add(key)

            if i % 5 == 0:
                logger.info("[%d] put '%s' OK  (total put: %d)", i, key, put_count)

            if i > 0 and i % 10 == 0:
                evicted = _check_eviction(store, existing_keys)
                if evicted:
                    evicted_keys.update(evicted)
                    existing_keys -= evicted
                    logger.info("EVICTION DETECTED at put #%d!", i)
                    logger.info("Evicted %d key(s): %s...", len(evicted), sorted(evicted)[:5])
                    logger.info("Total evicted so far: %d", len(evicted_keys))
                    break

        logger.info("=" * 60)
        logger.info("Summary:")
        logger.info("  Total put attempts:  %d", put_count)
        logger.info("  Keys still existing: %d", len(existing_keys))
        logger.info("  Keys evicted:        %d", len(evicted_keys))
        logger.info("  DRAM pool size:      %s", DRAM_SIZE)
        logger.info("  Block size:          %d bytes", BLOCK_SIZE)
        logger.info("  Approx data written: %.1f MB", put_count * BLOCK_SIZE / (1024 * 1024))
        logger.info("=" * 60)

    except KeyboardInterrupt:
        logger.info("Interrupted by user")
    finally:
        store.close()
        logger.info("LocalService closed")
        if START_META:
            _cleanup(proc)


def _check_eviction(store, keys):
    evicted = set()
    for key in keys:
        exists = store.is_exist(key)
        if exists == 0:
            evicted.add(key)
    return evicted


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
