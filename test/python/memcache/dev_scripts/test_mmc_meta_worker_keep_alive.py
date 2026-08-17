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
启动 1 个 MetaService + 1 个本地 worker（keep-alive）。

依赖环境变量：
    MMC_META_CONFIG_PATH  元数据服务配置（如 /home/config/mmc-meta.conf）
    MMC_LOCAL_CONFIG_PATH 本地服务配置   （如 /home/config/mmc-local.conf）

可杀性设计：
    主进程只做"监督者"，自身不导入 memcache_hybrid，因此不会被其 C++ 信号
    处理器接管 SIGINT/SIGTERM；meta 与 worker 都作为子进程运行，主进程收到
    Ctrl+C / SIGTERM 后用 SIGKILL 强制结束两个子进程再退出，保证一定可杀。
    注意：SIGKILL 不可被任何信号处理器拦截，worker 的 C++ 清理即使卡住也能退出。

行为：
    1. 子进程 meta 启动 MetaService（阻塞常驻）；
    2. 子进程 worker 初始化本地服务，写入 key1..keyN（每个 VALUE_SIZE 字节）；
    3. 写完后保持运行不退出，便于通过 meta HTTP 接口测试 /memory/evict、
       /memory/prefetch 等。
"""

import logging
import multiprocessing as mp
import signal
import time

KEY_COUNT = 20  # 写入的 key 个数：key1 .. keyN
VALUE_SIZE = 10 * 1024 * 1024  # 单个 value 大小：10MB
INIT_RETRY_TIMES = 60  # 等待 meta 就绪的最大重试次数
INIT_RETRY_INTERVAL = 2  # 每次重试间隔（秒）
DEVICE_ID = 1  # 使用的 NPU 卡号

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(processName)s %(levelname)s %(message)s",
)
logger = logging.getLogger("keepalive_meta_worker")


def run_meta():
    """子进程：常驻元数据服务。未调用 setup 时会读取 MMC_META_CONFIG_PATH。"""
    from memcache_hybrid import MetaService

    logger.info("MetaService starting ...")
    MetaService.main()
    logger.info("MetaService exited")


def run_worker():
    """子进程：worker。初始化本地服务、写入 key，然后保持存活。"""
    from memcache_hybrid import DistributedObjectStore

    store = DistributedObjectStore()
    last_err = None
    for attempt in range(1, INIT_RETRY_TIMES + 1):
        logger.info("attempt to init distributed store, attempt=%s", attempt)
        res = store.init(device_id=DEVICE_ID)
        if res == 0:
            logger.info("init ok (attempt %s)", attempt)
            break
        last_err = res
        logger.warning("init failed ret=%s, retry %s/%s ...", res, attempt, INIT_RETRY_TIMES)
        time.sleep(INIT_RETRY_INTERVAL)
    else:
        raise RuntimeError(f"init local service failed after {INIT_RETRY_TIMES} retries, last ret={last_err}")

    # 注意：DRAM 仅 64MB，key 总量超过 DRAM 时依赖淘汰策略，put 失败不中断，便于观察 evict
    value = bytearray(VALUE_SIZE)
    ok_count = 0
    for i in range(1, KEY_COUNT + 1):
        key = f"key{i}"
        ret = store.put(key, value)
        if ret == 0:
            ok_count += 1
            logger.info("put %s ok (%s bytes)", key, VALUE_SIZE)
        else:
            logger.warning("put %s failed ret=%s", key, ret)
    logger.info("done: %s/%s keys written, keep alive ...", ok_count, KEY_COUNT)

    # 保持进程存活，等待外部测试
    while True:
        time.sleep(3600)


def _sigterm_to_keyboardinterrupt(signum, frame):
    raise KeyboardInterrupt


def _kill_all(procs):
    """SIGKILL 强制结束所有子进程，绕过任何 C++ 信号处理器。"""
    for proc in procs:
        if proc.is_alive():
            logger.info("killing %s (SIGKILL)", proc.name)
            proc.kill()


if __name__ == "__main__":
    # 主进程不导入 memcache_hybrid，保持 Python 默认信号行为，SIGTERM 转成可捕获的异常
    signal.signal(signal.SIGTERM, _sigterm_to_keyboardinterrupt)

    meta_proc = mp.Process(target=run_meta, name="meta", daemon=True)
    worker_proc = mp.Process(target=run_worker, name="worker", daemon=True)
    procs = [meta_proc, worker_proc]
    for proc in procs:
        proc.start()

    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        logger.info("signal received, killing meta & worker ...")
    finally:
        _kill_all(procs)
