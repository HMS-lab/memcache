# DSV4-Flash+A3-PD分离（优选方案）

## 概述

[MemCache](https://gitcode.com/Ascend/memcache) 是针对AI推理场景设计的高性能分布式KVCache缓存系统，针对昇腾超节点、服务器的实现了精细化性能调优，提供较好的High Availability能力，同时对接到vllm-ascend、sglang、mindie等开源推理框架。

DeepSeek-V4 拥有百万字超长上下文，在 Agent 能力、世界知识和推理性能上均实现国内与开源领域的领先。相比 DeepSeek-V4-Pro，DeepSeek-V4-Flash 在世界知识储备方面稍逊一筹，但展现出了接近的推理能力。而由于模型参数和激活更小，相较之下 V4-Flash 能够提供更加快捷、经济的 API 服务。使用MemCache作为vLLM KV Pool后端，可以实现高效的KV Cache存储和请求间复用，从而大幅优化长文本、高复用（prefix cache）场景下的TTFT和吞吐水平。

## 测试结果

使用aisbench前缀测试工具验证：
工具使用文档：`https://github.com/rayn-zzz/aisbench_auto_tools_prefix/tree/main`

case1: 90%前缀，输入token数128K，输出token数1，并发60，共240条请求，其中有15个不同的前缀，每16个请求共享一个前缀.

`python3 aisbench_test.py --input_len 131072 --output_len 1 --data_num 240 --concurrency 60 --request_rate 0 --dataset_type prefix_cache --prefix_num 15 --repeat_rate 0.9 --prefix_test --dp 4`

测试结果：

| 是否开启memcache | mean ttft(ms) | mean tpot(ms) | HBM命中率 | DRAM命中率 |
|--|--|--|--|--|
| 否 | 51837.1 | / | 59.06% | / |
| 是 |  32121.5 | / | 57.24% | 70.76% |

## 环境说明

【硬件规格】：Atlas 800IA3 * 2

【模型权重】：[DeepSeek-V4-Flash-w8a8-mtp](https://www.modelscope.cn/models/Eco-Tech/DeepSeek-V4-Flash-w8a8-mtp)

<span style="color:#e60000;">注意：采用本文的部署方案必须满足以下底软版本要求</span>

【NPU驱动固件】>=25.5.1

【L1 1520 CC】>=1.5.0

【OS 内核版本】>=5.10

## 部署

### 1. 拉取vllm-ascend官方镜像

```shell
docker pull quay.io/ascend/vllm-ascend:v0.23.0rc1
```

启动容器的命令可参考 [vllm-ascend 部署 DeepSeek-V4-Flash 官方指导文档](https://docs.vllm.ai/projects/ascend/zh-cn/latest/tutorials/models/DeepSeek-V4-Flash.html) 。

<span style="color:#e60000;">**备注：** 当前v0.23.0rc1版本的vllm-ascend在A3上开启memcache作为内存池化后端时存在已知兼容性问题</span>，需要在镜像中将以下代码修改，或者通过git补丁的形式合入以规避此问题。目前vllm-ascend仓库中已修复此问题，预计下个release版本正式解决。

```diff
--- a/vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/pool_worker.py
+++ b/vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/pool_worker.py
@@ -737,8 +737,6 @@ class KVPoolWorker:
         )
         # Initialize store, register buffers, and start transfer threads
         # directly here (like main) — no separate init_backend handshake.
-        if hasattr(self.m_store, "init_store"):
-            self.m_store.init_store()
         self.m_store.register_buffer(ptrs, lengths)
         self._start_kv_transfer_threads()
```

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

MemCache由MetaService和LocalService两个组件构成，其中LocalService负责内存申请、数据读写和传输；MetaService作为独立进程启动，通过TCP与LocalService建立连接，负责元数据管理、数据淘汰、主备切换（高可用等功能）。

在A3环境上使用memcache时建议使用device_sdma协议配合[分离部署](https://gitcode.com/Ascend/memcache/wiki/MemCache+vLLM+A3%E5%88%86%E7%A6%BB%E9%83%A8%E7%BD%B2%E6%A1%88%E4%BE%8B.md)模式，达成最优性能。

启动时MetaService、LocalService的配置由各自配置文件导入，默认配置文件在memcache的whl包config目录下

```text
/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/
```

其中mmc-meta.conf为MetaService配置文件；mmc-local.conf为LocalService配置文件。

#### MetaService 配置文件

在任一节点上准备MetaService配置文件。

将以下 `<ip_or_domain>` 字段替换为实际环境中ip地址或域名，端口可根据需要修改，修改后需同步修改LocalService配置文件中的ip或域名以及端口。

```ini
## Meta service start-up url
ock.mmc.meta_service_url = tcp://<ip_or_domain>:5000

## Config store url
ock.mmc.meta_service.config_store_url = tcp://<ip_or_domain>:6000

## HTTP service url
ock.mmc.meta_service.metrics_url = http://127.0.0.1:8000

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## For more configuration options and advanced parameters, please refer to the official documentation: [MemCache Configuration Guide](https://gitcode.com/Ascend/memcache/blob/master/doc/memcache_config.md)
```

#### LocalService 配置文件

在所有节点上准备LocalService配置文件。

```ini
## Meta service start-up url
## Keep consistent with the same name configuration in mmc-meta.conf
ock.mmc.meta_service_url = tcp://<ip_or_domain>:5000

## Config store url
## Keep consistent with the same name configuration in mmc-meta.conf
ock.mmc.local_service.config_store_url = tcp://<ip_or_domain>:6000

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## The maximum supported rank count; once ranks are connected, no further modifications are allowed — a meta restart is required
ock.mmc.local_service.world_size = 256

## Data transfer protocol options
ock.mmc.local_service.protocol = device_sdma

## DRAM space usage
## 根据实际情况修改dram的size
ock.mmc.local_service.dram.size = 100GB

## max DRAM space usage
ock.mmc.local_service.max.dram.size = 1024GB

## For more configuration options and advanced parameters, please refer to the official documentation: [MemCache Configuration Guide](https://gitcode.com/Ascend/memcache/blob/master/doc/memcache_config.md)
```

分离部署模式每个节点需准备两份配置文件，分别给独立LocalService进程以及和VLLM推理进程集成的Client使用。其中独立LocalService进程负责贡献内存。VLLM集成的Client进程负责数据读写和传输等。两份配置文件区别如下，其他字段须保持一致：

##### 独立LocalService进程配置文件

```ini
## DRAM space usage
## 根据实际情况修改dram的size
ock.mmc.local_service.dram.size = 100GB
```

##### VLLM推理进程Client配置文件

```ini
## DRAM space usage
## dram的size设置为0，不贡献内存
ock.mmc.local_service.dram.size = 0
```

建议分别保存为`mmc-local-standalone.conf`和`mmc-local.conf`.

#### 准备独立LocalService进程启动脚本

在每个节点上准备如下脚本，假设命名为`memcache_standalone.py`.

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

更多配置可以参考[官方配置文档](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/memcache_config.md).

### 4. 准备推理服务启动相关脚本

本节展示了在Atlas 800 A3 双节点环境下，以1P1D配置部署DeepSeek-V4-Flash以获得更优性能的指南。

#### 4.1 在每个节点上准备脚本 launch_online_dp.py

```python
import argparse
import multiprocessing
import os
import subprocess
import sys

def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--dp-size",
        type=int,
        required=True,
        help="Data parallel size."
    )
    parser.add_argument(
        "--tp-size",
        type=int,
        default=1,
        help="Tensor parallel size."
    )
    parser.add_argument(
        "--dp-size-local",
        type=int,
        default=-1,
        help="Local data parallel size."
    )
    parser.add_argument(
        "--dp-rank-start",
        type=int,
        default=0,
        help="Starting rank for data parallel."
    )
    parser.add_argument(
        "--dp-address",
        type=str,
        required=True,
        help="IP address for data parallel master node."
    )
    parser.add_argument(
        "--dp-rpc-port",
        type=str,
        default=12345,
        help="Port for data parallel master node."
    )
    parser.add_argument(
        "--vllm-start-port",
        type=int,
        default=9000,
        help="Starting port for the engine."
    )
    return parser.parse_args()

args = parse_args()
dp_size = args.dp_size
tp_size = args.tp_size
dp_size_local = args.dp_size_local
if dp_size_local == -1:
    dp_size_local = dp_size
dp_rank_start = args.dp_rank_start
dp_address = args.dp_address
dp_rpc_port = args.dp_rpc_port
vllm_start_port = args.vllm_start_port

def run_command(visible_devices, dp_rank, vllm_engine_port):
    command = [
        "bash",
        "./run_dp_template.sh",
        visible_devices,
        str(vllm_engine_port),
        str(dp_size),
        str(dp_rank),
        dp_address,
        dp_rpc_port,
        str(tp_size),
    ]
    subprocess.run(command, check=True)

if __name__ == "__main__":
    template_path = "./run_dp_template.sh"
    if not os.path.exists(template_path):
        print(f"Template file {template_path} does not exist.")
        sys.exit(1)

    processes = []
    num_cards = dp_size_local * tp_size
    for i in range(dp_size_local):
        dp_rank = dp_rank_start + i
        vllm_engine_port = vllm_start_port + i
        visible_devices = ",".join(str(x) for x in range(i * tp_size, (i + 1) * tp_size))
        process = multiprocessing.Process(target=run_command,
                                        args=(visible_devices, dp_rank,
                                                vllm_engine_port))
        processes.append(process)
        process.start()

    for process in processes:
        process.join()
```

#### 4.2 P节点推理服务启动脚本 run_dp_template.sh

```bash
unset ftp_proxy
unset https_proxy
unset http_proxy

nic_name="xxxx" # 改成实际网卡名称
local_ip=xx.xx.xx.1 # 改成实际网卡IP

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export VLLM_RPC_TIMEOUT=3600000
export VLLM_EXECUTE_MODEL_TIMEOUT_SECONDS=30000
export HCCL_EXEC_TIMEOUT=204
export HCCL_CONNECT_TIMEOUT=120

export OMP_PROC_BIND=false
export OMP_NUM_THREADS=10
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=2560
export TASK_QUEUE_ENABLE=1

export VLLM_ASCEND_ENABLE_FLASHCOMM1=1
export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export HCCL_OP_EXPANSION_MODE="AIV"
export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2:$LD_PRELOAD
export ASCEND_RT_VISIBLE_DEVICES=$1

export PYTHONHASHSEED=0
# 修改配置文件为实际路径，此处为示例
export MMC_LOCAL_CONFIG_PATH=/xxx/mmc-local.conf
# 此环境变量可减少部分kvcache读写量，在某些场景下可优化性能，但也可能影响前缀命中率，建议实测按需设置。
# 不设置为默认行为，设置0为最激进策略，其他值可设置为最小可命中token数的倍数，以dsv4模型block_size=128为例，可以设置为16384的倍数。
# vllm环境变量参考https://docs.vllm.com.cn/en/latest/configuration/env_vars/?h=environment
export VLLM_PREFIX_CACHE_RETENTION_INTERVAL=0

# 修改权重路径为实际路径，此处为示例
vllm serve /root/.cache/modelscope/hub/models/vllm-ascend/DeepSeek-V4-Flash-w8a8-mtp \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --seed 1024 \
    --served-model-name dsv4 \
    --max-model-len 1048576 \
    --max-num-batched-tokens 8192 \
    --max-num-seqs 16 \
    --no-disable-hybrid-kv-cache-manager \
    --model-loader-extra-config='{"enable_multithread_load": "true", "num_threads": 128}' \
    --safetensors-load-strategy 'prefetch' \
    --speculative-config '{"num_speculative_tokens": 1,"method": "mtp","enforce_eager": true}' \
    --trust-remote-code \
    --block-size 128 \
    --tokenizer-mode deepseek_v4 \
    --tool-call-parser deepseek_v4 \
    --enable-auto-tool-choice \
    --reasoning-parser deepseek_v4 \
    --gpu-memory-utilization 0.85 \
    --quantization ascend \
    --enforce-eager \
    --additional-config '{"enable_cpu_binding": true, "enable_shared_expert_dp": true,  "enable_dsa_cp": true}' \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_producer",
        "kv_connector_extra_config": {
            "connectors": [
                {
                    "kv_connector": "MooncakeHybridConnector",
                    "kv_role": "kv_producer",
                    "kv_port": "30000",
                    "kv_connector_extra_config": {
                        "prefill": {
                            "dp_size": 4,
                            "tp_size": 4
                        },
                        "decode": {
                            "dp_size": 16,
                            "tp_size": 1
                        }
                    }
                },
                {
                    "kv_connector": "AscendStoreConnector",
                    "kv_role": "kv_producer",
                    "kv_connector_extra_config": {
                        "lookup_rpc_port":"0",
                        "backend": "memcache"
                    }
                }
            ]
        }
    }' 2>&1 | tee pd_p1.log
```

#### 4.3 D节点推理服务启动脚本 run_dp_template.sh

```bash
unset ftp_proxy
unset https_proxy
unset http_proxy

nic_name="xxxx" # 改成实际网卡名称
local_ip=xx.xx.xx.2 # 改成实际网卡IP

export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2:$LD_PRELOAD
export HCCL_OP_EXPANSION_MODE="AIV"
export TASK_QUEUE_ENABLE=1

export VLLM_RPC_TIMEOUT=3600000
export VLLM_EXECUTE_MODEL_TIMEOUT_SECONDS=30000
export HCCL_EXEC_TIMEOUT=204
export HCCL_CONNECT_TIMEOUT=1200

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export OMP_PROC_BIND=false
export OMP_NUM_THREADS=10
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=1024

export ASCEND_RT_VISIBLE_DEVICES=$1
export PYTHONHASHSEED=0
# 修改配置文件为实际路径，此处为示例
export MMC_LOCAL_CONFIG_PATH=/xxx/mmc-local.conf
# 此环境变量可减少部分kvcache读写量，在某些场景下可优化性能，但也可能影响前缀命中率，建议实测按需设置。
# 不设置为默认行为，设置0为最激进策略，其他值可设置为最小可命中token数的倍数，以dsv4模型block_size=128为例，可以设置为16384的倍数。
# vllm环境变量参考https://docs.vllm.com.cn/en/latest/configuration/env_vars/?h=environment
export VLLM_PREFIX_CACHE_RETENTION_INTERVAL=0

# 修改权重路径为实际路径，此处为示例
vllm serve /root/.cache/modelscope/hub/models/vllm-ascend/DeepSeek-V4-Flash-w8a8-mtp \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --seed 1024 \
    --served-model-name dsv4 \
    --max-model-len 1048576 \
    --max-num-batched-tokens 120 \
    --max-num-seqs 60 \
    --async-scheduling \
    --block-size 128 \
    --no-disable-hybrid-kv-cache-manager \
    --safetensors-load-strategy 'prefetch' \
    --trust-remote-code \
    --tokenizer-mode deepseek_v4 \
    --model-loader-extra-config='{"enable_multithread_load": "true", "num_threads": 128}' \
    --tool-call-parser deepseek_v4 \
    --enable-auto-tool-choice \
    --reasoning-parser deepseek_v4 \
    --gpu-memory-utilization 0.9 \
    --quantization ascend \
    --speculative-config '{"num_speculative_tokens": 1,"method": "mtp","enforce_eager": true}' \
    --compilation-config '{"cudagraph_mode": "FULL_DECODE_ONLY"}' \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_consumer",
        "kv_connector_extra_config": {
            "connectors": [
                {
                    "kv_connector": "MooncakeHybridConnector",
                    "kv_role": "kv_consumer",
                    "kv_port": "30100",
                    "kv_connector_extra_config": {
                        "prefill": {
                            "dp_size": 4,
                            "tp_size": 4
                        },
                        "decode": {
                            "dp_size": 16,
                            "tp_size": 1
                        }
                    }
                },
                {
                    "kv_connector": "AscendStoreConnector",
                    "kv_role": "kv_consumer",
                    "kv_connector_extra_config": {
                        "lookup_rpc_port":"0",
                        "backend": "memcache"
                    }
                }
            ]
        }
    }' \
    --additional-config '{
        "ascend_compilation_config":{
            "enable_npugraph_ex":true,
            "enable_static_kernel":false
        },
        "enable_cpu_binding":true,
        "multistream_overlap_shared_expert":true,
        "recompute_scheduler_enable":true
    }' 2>&1 | tee pd_d.log

```

#### 4.4 请求转发代理程序脚本

在任一节点上准备以下文件：

从vllm-ascend仓库中获取代理程序[load_balance_proxy_server_example.py](https://github.com/vllm-project/vllm-ascend/blob/main/examples/disaggregated_prefill_v1/load_balance_proxy_server_example.py)

再准备启动此py脚本的bash脚本，假设命名为`start_proxy.sh`（需要修改其中`<proxy_ip>`、`<p_ip>`、`<d_ip>`为服务实际启动的ip地址，`<proxy_ip>`为启动proxy的节点ip，`<p_ip>`、`<d_ip>`分别为p、d节点的ip）

```bash
unset http_proxy
unset https_proxy
python load_balance_proxy_server_example.py \
  --port 8025 \
  --host <proxy_ip> \
  --prefiller-hosts \
    <p_ip> \
    <p_ip> \
    <p_ip> \
    <p_ip> \
  --prefiller-ports \
    7100 7101 7102 7103 \
  --decoder-hosts \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
    <d_ip> \
  --decoder-ports \
    7100 7101 7102 7103 7104 7105 7106 7107 7108 7109 7110 7111 7112 7113 7114 7115 \
```

### 5. 拉起服务

#### 5.1 拉起MetaService服务

在任一节点拉起MetaService元数据管理服务，配置文件路径修改为实际路径

```bash
export MMC_META_CONFIG_PATH=/xxx/mmc-meta.conf
python3 -c "from memcache_hybrid import MetaService; MetaService.main()"
```

#### 5.2 拉起LocalService独立进程

在所有节点拉起LocalService独立进程，配置文件路径修改为实际路径

```bash
export MMC_LOCAL_CONFIG_PATH="/xxx/mmc-local-standalone.conf"
python3 memcache_standalone.py
```

等待所有节点都启动成功（看到打印"Successfully initialized memcache. "）后再进行下一步。

#### 5.3 拉起VLLM推理服务

需修改其中 `<p_ip>` 和 `<d_ip>` 为p、d节点的ip，如需修改端口请同步修改请求转发代理程序脚本`start_proxy.sh`.

P节点：

```bash
python launch_online_dp.py --dp-size 4 --tp-size 4 --dp-size-local 4 --dp-rank-start 0 --dp-address <p_ip> --dp-rpc-port 12321 --vllm-start-port 7100
```

D节点：

```bash
python launch_online_dp.py --dp-size 16 --tp-size 1 --dp-size-local 16 --dp-rank-start 0 --dp-address <d_ip> --dp-rpc-port 12321 --vllm-start-port 7100
```

#### 5.4 拉起请求转发代理程序脚本

在任一节点拉起

```bash
bash start_proxy.sh
```
