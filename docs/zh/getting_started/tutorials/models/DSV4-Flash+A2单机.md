# DSV4-Flash+A2单机

## 概述

[MemCache](https://gitcode.com/Ascend/memcache) 是针对AI推理场景设计的高性能分布式KVCache缓存系统，针对昇腾超节点、服务器的实现了精细化性能调优，提供较好的High Availability能力，同时对接到vllm-ascend、sglang、mindie等开源推理框架。

DeepSeek-V4 拥有百万字超长上下文，在 Agent 能力、世界知识和推理性能上均实现国内与开源领域的领先。相比 DeepSeek-V4-Pro，DeepSeek-V4-Flash 在世界知识储备方面稍逊一筹，但展现出了接近的推理能力。而由于模型参数和激活更小，相较之下 V4-Flash 能够提供更加快捷、经济的 API 服务。使用MemCache作为vLLM KV Pool后端，可以实现高效的KV Cache存储和请求间复用，从而大幅优化长文本、高复用（prefix cache）场景下的TTFT和吞吐水平。

## 环境说明

【硬件规格】：A2 * 1

【模型权重】：[DeepSeek-V4-Flash-w8a8-mtp](https://www.modelscope.cn/models/Eco-Tech/DeepSeek-V4-Flash-w8a8-mtp)

## 部署

### 1. 拉取vllm-ascend官方镜像

```shell
docker pull quay.io/ascend/vllm-ascend:v0.23.0rc1
```

启动容器的命令可参考 [vllm-ascend 部署 DeepSeek-V4-Flash 官方指导文档](https://docs.vllm.ai/projects/ascend/zh-cn/latest/tutorials/models/DeepSeek-V4-Flash.html) 。

### 2. 安装MemCache

vllm-ascend官方镜像中已安装memcache及其配套，可通过如下方式查询已有版本：

```shell
pip list | grep mem
```

目前推荐的稳定版本为1.1.5，安装命令为：

```shell
pip install memfabric_hybrid==1.1.5
pip install memcache_hybrid==1.1.5
```

如果因网络原因无法在线安装，可以在 [PyPI · The Python Package Index](https://pypi.org/) 下载安装包离线安装。

下载链接：

[memfabric](https://pypi.org/project/memfabric-hybrid/#history)

[memcache](https://pypi.org/project/memcache-hybrid/#history)

安装其他非正式发布版可以直接源码编译安装，参考 [官方编译文档](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/install_run.md) 即可。

### 3. 修改 MemCache 配置文件

MemCache由MetaService和LocalService两个组件构成，其中LocalService集成到推理框架中随推理进程一同启动，负责内存申请、数据读写和传输，MetaService作为独立进程启动，通过TCP与LocalService建立连接，负责元数据管理、数据淘汰、主备切换（高可用等功能）。

启动时MetaService、LocalService的配置由各自配置文件导入，默认配置文件在memcache的whl包config目录下

```text
/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/
```

#### MetaService 配置文件

```ini
## Meta service start-up url
ock.mmc.meta_service_url = tcp://127.0.0.1:5000

## Config store url
ock.mmc.meta_service.config_store_url = tcp://127.0.0.1:6000

## HTTP service url
ock.mmc.meta_service.metrics_url = http://127.0.0.1:8000

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## For more configuration options and advanced parameters, please refer to the official documentation: [MemCache Configuration Guide](https://gitcode.com/Ascend/memcache/blob/master/doc/memcache_config.md)
```

#### LocalService 配置文件

```ini
## Meta service start-up url
## Keep consistent with the same name configuration in mmc-meta.conf
ock.mmc.meta_service_url = tcp://127.0.0.1:5000

## Config store url
## Keep consistent with the same name configuration in mmc-meta.conf
ock.mmc.local_service.config_store_url = tcp://127.0.0.1:6000

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## The maximum supported rank count; once ranks are connected, no further modifications are allowed — a meta restart is required
ock.mmc.local_service.world_size = 256

## Data transfer protocol options
ock.mmc.local_service.protocol = host_shm

## DRAM space usage
## 根据实际情况修改单卡dram的size
ock.mmc.local_service.dram.size = 80GB

## For more configuration options and advanced parameters, please refer to the official documentation: [MemCache Configuration Guide](https://gitcode.com/Ascend/memcache/blob/master/doc/memcache_config.md)
```

### 4. 准备vLLM启动脚本

```shell
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=10
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2:$LD_PRELOAD
export HCCL_BUFFSIZE=1024
export VLLM_ASCEND_ENABLE_FLASHCOMM1=1
export TASK_QUEUE_ENABLE=1
export HCCL_OP_EXPANSION_MODE="AIV"

# 此环境变量可减少部分kvcache读写量，在某些场景下可优化性能，但也可能影响前缀命中率，建议实测按需设置。
# 不设置为默认行为，设置0为最激进策略，其他值可设置为最小可命中token数的倍数，以dsv4模型block_size=128为例，可以设置为16384的倍数。
# vllm环境变量参考https://docs.vllm.com.cn/en/latest/configuration/env_vars/?h=environment
export VLLM_PREFIX_CACHE_RETENTION_INTERVAL=0

export MMC_LOCAL_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf

vllm serve /home/weights/DeepSeek-V4-Flash-w8a8-mtp \
    --port 8900 \
    --max_model_len 133120 \
    --max-num-batched-tokens 8192 \
    --served-model-name dsv4 \
    --gpu-memory-utilization 0.9 \
    --block-size 128 \
    --max-num-seqs 32 \
    --data-parallel-size 1 \
    --tensor-parallel-size 8 \
    --enable-expert-parallel \
    --tokenizer-mode deepseek_v4 \
    --tool-call-parser deepseek_v4 \
    --enable-auto-tool-choice \
    --reasoning-parser deepseek_v4 \
    --safetensors-load-strategy 'prefetch' \
    --model-loader-extra-config='{"enable_multithread_load": "true", "num_threads": 128}' \
    --quantization ascend \
    --speculative-config '{"num_speculative_tokens": 1, "method": "mtp", "enforce_eager": true}' \
    --compilation-config '{"cudagraph_mode": "FULL_DECODE_ONLY"}'\
    --async-scheduling \
    --no-disable-hybrid-kv-cache-manager \
    --kv-transfer-config \
    '{
        "kv_connector": "AscendStoreConnector",
        "kv_role": "kv_both",
        "kv_connector_extra_config": {
            "backend": "memcache",
            "mooncake_rpc_port":"0"
        },
        "kv_load_failure_policy": "recompute"
    }' \
    --additional-config '
    {
        "ascend_compilation_config": {
            "enable_npugraph_ex": true,
            "enable_static_kernel": false
        },
        "enable_cpu_binding": true,
        "enable_dsa_cp": true,
        "multistream_overlap_shared_expert": true
    }' 2>&1 | tee output.log
```

**相比基线脚本，使能Memcache模式下仅涉及以下变化：**

1. 添加了环境变量导入MemCache LocalService配置文件路径（这里给的是whl包默认配置文件路径）

    ```shell
    export MMC_LOCAL_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf
    ```

2. 添加了 kv-transfer-config

    ```shell
    --kv-transfer-config \
        '{
            "kv_connector": "AscendStoreConnector",
            "kv_role": "kv_both",
            "kv_connector_extra_config": {
                "backend": "memcache"
            },
            "kv_load_failure_policy": "recompute"
        }'
    ```

### 5. 拉起服务

#### 5.1 在每个节点配置大页内存

host_shm不依赖大页也可以拉起，但是走4K页性能远不如静态大页，建议使用静态大页的方式拉起服务，以获得最佳性能

宿主机上通过如下命令查询大页配置，获取Hugepagesize，计算需要配置的大页数量

```shell
grep -i huge /proc/meminfo
```

计算方法为

```ini
total_hugepages_number = dram_size_per_rank * 8 / Hugepagesize
```

例如，每卡配置100GB内存，大页大小为2MB，则总共需要大页数量为 `100*8*1024/2=409600`。

通过如下命令设置大页数量

```shell
echo 409600 > /proc/sys/vm/nr_hugepages
```

如果是非root用户可使用如下命令

```shell
echo 409600 | sudo tee /proc/sys/vm/nr_hugepages
```

**tips：如果在容器或POD中使用，需要保证容器内映射了 `/dev/hugepages/`目录**

#### 5.2 拉起MetaService元数据管理服务

```shell
export MMC_META_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-meta.conf
python3 -c "from memcache_hybrid import MetaService; MetaService.main()"
```

#### 5.3 使用脚本拉起推理服务

## 测试

使用aisbench前缀测试工具验证：
case1: 50%前缀，输入token数40K，输出token数200，并发32，共128条请求，其中有32个不同的前缀，每4个请求共享一个前缀

测试结果：

| 是否开启memcache | mean ttft(ms) | mean tpot(ms) | HBM命中率 | DRAM命中率 |
|--|--|--|--|--|
| 否 | 24735 | 226.9 | 7.46% | / |
| 是 | 11696 | 238.3 | 7.11% | 35.13% |

case2: 90%前缀，输入token数128K，输出token数1，并发16，共64条请求，其中有16个不同的前缀，每4个请求共享一个前缀

测试结果：

| 是否开启memcache | mean ttft(ms) | mean tpot(ms) | HBM命中率 | DRAM命中率 |
|--|--|--|--|--|
| 否 | 109819 | / | 0% | / |
| 是 | 34991 | / | 0% | 87.5% |
