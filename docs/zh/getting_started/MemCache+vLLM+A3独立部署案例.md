# MemCache+vLLM+A3独立部署案例

## 概述

A3 环境中，由于[地址交集](https://gitcode.com/Ascend/memfabric_hybrid/wiki/FAQ.md#2-%E8%B0%83%E7%94%A8halmemcreate%E8%BF%94%E5%9B%9E%E9%94%99%E8%AF%AF%E7%A0%81-6)的限制，2T内存环境使用device_sdma只能使用约680GB内存做池化

在`融合部署模式`（MemCache和vllm在同一个进程）下，由于推理框架首先加载权重，然后才初始化kvconnector进而初始化MemCache，导致MemCache无法申请到足够内存空间，[典型报错FAQ](https://gitcode.com/Ascend/memfabric_hybrid/wiki/FAQ.md#2-%E8%B0%83%E7%94%A8halmemcreate%E8%BF%94%E5%9B%9E%E9%94%99%E8%AF%AF%E7%A0%81-6)。

此时可以采用`分离部署模式`（MemCache和vllm不在同一个进程）申请尽量大的内存空间:

第一步：在拉起推理进程前，首先在每个节点启动memcache独立进程，即由独立的进程贡献内存。

第二步：拉起推理进程，此时推理进程中的memcache不贡献内存。

部署示意图如下：
![memcache_separated_deployment.png](https://raw.gitcode.com/user-images/assets/7672915/faac25cd-1e48-4d5d-9649-eb5fad1850b3/memcache_separated_deployment.png 'memcache_separated_deployment.png')

## 操作方式

本文假定读者已经掌握了常规方式拉起推理框架+memcache的能力。

### 1. 扫描确认环境sdma可用内存量

确保 free -h 所示的 “buff/cache”项占用尽量少

![image.png](https://raw.gitcode.com/user-images/assets/7672915/838d16fc-206d-4bad-9488-12d71cc93a49/image.png 'image.png')

可以通过下列命令清掉文件缓存<span style="color:#e60000;">！！！注意！！！</span>此命令为清缓存命令，会导致权重等缓存需要从文件系统重新加载，再次拉起vllm框架会变慢，请熟悉其背后代表的含义判断是否使用

```shell
echo 3 > /proc/sys/vm/drop_caches
```

运行[mem_scan.py](https://gitcode.com/Ascend/memfabric_hybrid/blob/master/src/smem/python/memfabric_hybrid/memfabric_hybrid/mem_scan.py)脚本扫描可用的物理内存：

`python3 mem_scan.py`

![image.png](https://raw.gitcode.com/user-images/assets/7672916/348c0854-4efb-4803-8222-f6bf19589fa1/image.png 'image.png')

### 2. 准备配置文件

假设在上一步中扫描得知环境上至少有600GB可用内存。

首先修改推理进程使用的mmc-local.conf，将ock.mmc.local_service.dram.size修改为0，然后增加配置项 ock.mmc.local_service.max.dram.size = 1024GB

```ini
# 修改配置
ock.mmc.local_service.dram.size = 0GB

# 增加配置
ock.mmc.local_service.max.dram.size = 1024GB
```

然后复制一份推理进程的配置文件给memcache独立进程使用，假设命名为"mmc-local-standalone.conf"（可以任意命名），并将 ock.mmc.local_service.dram.size 修改为独立进程内存申请量。

```ini
# 修改配置
ock.mmc.local_service.dram.size = 600GB

# 增加配置
ock.mmc.local_service.max.dram.size = 1024GB
```

### 3. 准备脚本

在每个节点上准备如下脚本，假设命名为 memcache_standalone.py

```python
import time

from memcache_hybrid import DistributedObjectStore


if __name__ == "__main__":
    store = DistributedObjectStore()
    res = store.init(0)
    if res != 0:
        print(f"Failed to initialize memcache, res = {res}", flush=True)
        exit(1)
    print("Successfully initialized memcache. ", flush=True)

    while True:
        time.sleep(1)
```

### 4. 拉起

1. 首先按原有方案拉起metaservice。

   ```shell
   export MMC_META_CONFIG_PATH=[mmc-meta.conf PATH]
   python3 -c 'from memcache_hybrid import MetaService; MetaService.main()'
   ```

2. 然后拉起memcache独立进程。

   ```shell
   export MMC_LOCAL_CONFIG_PATH="mmc-local-standalone.conf"
   python3 memcache_standalone.py
   ```

   **等待所有节点都启动成功（看到打印"Successfully initialized memcache. "）后再进行下一步**

3. 最后按原有方法拉起推理进程。
