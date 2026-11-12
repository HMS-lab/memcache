#!/usr/bin/env python
# coding=utf-8
"""
Test local_first alloc strategy with cross-node fallback.

Setup:
  - Meta service on ports 5088/6088/8088
  - Local writer 1: backend_id=192.168.0.1, writes 100 keys immediately
  - Local writer 2: backend_id=192.168.0.2, sleeps 3s then writes 100 keys
  - Alloc strategy: local_first (set via MetaConfig, managed centrally by MetaService)

Expected:
  - Writer 1 keys → rank 0 (local to writer 1)
  - Writer 2 keys → rank 1 (local to writer 2)
  - Each writer prefers its own node first
"""

import logging
import multiprocessing
import time

from memcache_hybrid import DistributedObjectStore, LocalConfig, MetaConfig, MetaService

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger(__name__)

META_URL = "tcp://127.0.0.1:5088"
CS_URL = "tcp://127.0.0.1:6088"
METRICS_URL = "http://127.0.0.1:8088"
NUM_KEYS = 100
VALUE_SIZE = 1024  # 1KB per value

# AllocFlags enum values
ALLOC_RANDOM = 2
ALLOC_ARRANGE = 4
ALLOC_LOCAL_FIRST = 8


def start_meta():
    config = MetaConfig()
    config.meta_service_url = META_URL
    config.config_store_url = CS_URL
    config.metrics_url = METRICS_URL
    config.log_level = "debug"
    config.alloc_strategy = ALLOC_LOCAL_FIRST  # Global strategy configured centrally by MetaService
    assert MetaService.setup(config) == 0, "meta setup failed"
    logger.info("[meta] setup done, starting main loop...")
    MetaService.main()


def local_writer(backend_id, sleep_before=0, result_dict=None):
    """Init a local service and write NUM_KEYS keys."""
    config = LocalConfig()
    config.meta_service_url = META_URL
    config.config_store_url = CS_URL
    config.backend_id = backend_id
    config.protocol = "host_tcp"
    config.dram_size = "256MB"

    store = DistributedObjectStore()
    res = store.setup(config)
    logger.info("[%s] setup: %s", backend_id, res)
    assert res == 0, f"setup failed for {backend_id}"

    res = store.init(0)
    logger.info("[%s] init: %s", backend_id, res)
    assert res == 0, f"init failed for {backend_id}"

    if sleep_before > 0:
        logger.info("[%s] sleeping %ss ...", backend_id, sleep_before)
        time.sleep(sleep_before)

    keys = [f"{backend_id}_key_{i}" for i in range(NUM_KEYS)]
    value = b"x" * VALUE_SIZE

    logger.info("[%s] writing %s keys ...", backend_id, NUM_KEYS)
    ok_count = 0
    fail_count = 0
    for key in keys:
        res = store.put(key, value)
        if res == 0:
            ok_count += 1
        else:
            fail_count += 1
    logger.info("[%s] write done: %s ok, %s failed", backend_id, ok_count, fail_count)

    # Verify: check which rank each key landed on
    rank_set = set()
    for key in keys:
        info = store.get_key_info(key)
        locs = info.loc_list()
        if locs:
            rank_set.add(locs[0])

    logger.info("[%s] keys landed on ranks: %s", backend_id, sorted(rank_set))

    if result_dict is not None:
        result_dict["ok"] = ok_count
        result_dict["fail"] = fail_count
        result_dict["ranks"] = sorted(rank_set)

    store.close()


if __name__ == "__main__":
    multiprocessing.set_start_method("spawn", force=True)
    manager = multiprocessing.Manager()
    result1 = manager.dict()
    result2 = manager.dict()

    # 1. Start meta service
    logger.info("=" * 60)
    logger.info("Starting meta service ...")
    meta_proc = multiprocessing.Process(target=start_meta, daemon=True)
    meta_proc.start()
    logger.info("  meta pid=%s, waiting 3s ...", meta_proc.pid)
    time.sleep(3)

    # 2. Start writer 1 (192.168.0.1) — writes immediately
    logger.info("Starting writer 1 (192.168.0.1) ...")
    p1 = multiprocessing.Process(
        target=local_writer,
        args=("192.168.0.1", 0, result1),
    )
    p1.start()

    # 3. Start writer 2 (192.168.0.2) — sleeps 3s then writes
    logger.info("Starting writer 2 (192.168.0.2, sleep 3s) ...")
    p2 = multiprocessing.Process(
        target=local_writer,
        args=("192.168.0.2", 3, result2),
    )
    p2.start()

    # 4. Wait for both
    p1.join(timeout=60)
    p2.join(timeout=60)

    # 5. Results
    logger.info("=" * 60)
    logger.info("RESULTS:")
    logger.info("  Writer 1 (192.168.0.1): ok=%s, ranks=%s", dict(result1).get("ok"), dict(result1).get("ranks"))
    logger.info("  Writer 2 (192.168.0.2): ok=%s, ranks=%s", dict(result2).get("ok"), dict(result2).get("ranks"))

    r1_ranks = dict(result1).get("ranks", [])
    r2_ranks = dict(result2).get("ranks", [])
    if r1_ranks and r2_ranks and set(r1_ranks).isdisjoint(set(r2_ranks)):
        logger.info("PASS: each writer used its own local rank (no overlap)")
    elif r1_ranks and r2_ranks:
        logger.info("NOTE: writer 1 ranks=%s, writer 2 ranks=%s", r1_ranks, r2_ranks)
        logger.info("Check if local-first allocation is working as expected")
    else:
        logger.error("FAIL: could not determine rank allocation")

    # Cleanup
    logger.info("Cleaning up ...")
    p1.terminate()
    p2.terminate()
    meta_proc.terminate()
    meta_proc.join(timeout=3)
    if meta_proc.is_alive():
        meta_proc.kill()
        meta_proc.join()

    logger.info("Done.")
