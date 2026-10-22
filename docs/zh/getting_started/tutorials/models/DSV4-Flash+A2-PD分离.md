# DSV4-Flash+A2-PD分离

## 概述

[MemCache](https://gitcode.com/Ascend/memcache) 是针对AI推理场景设计的高性能分布式KVCache缓存系统，针对昇腾超节点、服务器的实现了精细化性能调优，提供较好的High Availability能力，同时对接到vllm-ascend、sglang、mindie等开源推理框架。

DeepSeek-V4 拥有百万字超长上下文，在 Agent 能力、世界知识和推理性能上均实现国内与开源领域的领先。相比 DeepSeek-V4-Pro，DeepSeek-V4-Flash 在世界知识储备方面稍逊一筹，但展现出了接近的推理能力。而由于模型参数和激活更小，相较之下 V4-Flash 能够提供更加快捷、经济的 API 服务。使用MemCache作为vLLM KV Pool后端，可以实现高效的KV Cache存储和请求间复用，从而大幅优化长文本、高复用（prefix cache）场景下的TTFT和吞吐水平。

## 环境说明

【硬件规格】：Atlas 800 A2（64GB × 8）4机

【部署配置】：2 \* 1P + 1 \* 2D

【模型权重】：[DeepSeek-V4-Flash-w8a8-mtp](https://www.modelscope.cn/models/Eco-Tech/DeepSeek-V4-Flash-w8a8-mtp)

<span style="color:#e60000;">注意：采用本文的部署方案必须满足以下底软要求</span>

【NPU驱动固件】：不低于25.5.1

【OS PAGESIZE】：使用命令 `getconf PAGESIZE` 查询系统页大小，结果为4K方可使用memcache。若查询结果为64K，需要考虑更换OS。

## 测试结果

![ttft_compare.png](https://raw.gitcode.com/user-images/assets/7672915/f57f146e-c0be-4bda-ab7a-fefb6571d1cc/ttft_compare.png 'ttft_compare.png')

详细数据：

| 池化 | 总请求数 | 并发 | 前缀数 | 输入长度 | 输出长度 | 前缀重复率 | 理论命中率 | 实际命中率 | TTFT(ms) | TPOT(ms) |
| ---- | -------- | ---- | ------ | -------- | -------- | ---------- | ---------- | ---------- | -------- | -------- |
| 关闭 | 200      | 40   | 40     | 64K      | 1K       | 80%        | 64%        | 12.15%     | 52787.2  | 24.5     |
| 关闭 | 250      | 50   | 50     | 64K      | 1K       | 80%        | 64%        | 13.80%     | 70080.6  | 24.3     |
| 关闭 | 300      | 60   | 60     | 64K      | 1K       | 80%        | 64%        | 8.25%      | 101942.1 | 24.0     |
| 启用 | 200      | 40   | 40     | 64K      | 1K       | 80%        | 64%        | 59.5%      | 22184.7  | 25.9     |
| 启用 | 250      | 50   | 50     | 64K      | 1K       | 80%        | 64%        | 59.8%      | 25809.9  | 26.4     |
| 启用 | 300      | 60   | 60     | 64K      | 1K       | 80%        | 64%        | 59.9%      | 33407.4  | 26.5     |

测试工具：aisbench

## 部署步骤

### 1. 拉取vllm-ascend官方镜像

拉取vllm-ascend官方镜像

```shell
docker pull quay.io/ascend/vllm-ascend:v0.23.0
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

- [memfabric](https://pypi.org/project/memfabric-hybrid/#history)

- [memcache](https://pypi.org/project/memcache-hybrid/#history)

安装其他非正式发布版可以直接源码编译安装，参考 [官方编译文档](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/install_run.md) 即可。

### 3. 准备MemCache配置文件

MemCache由MetaService和LocalService两个组件构成，其中LocalService负责内存申请、数据读写和传输，MetaService作为独立进程启动，通过TCP与LocalService建立连接，负责元数据管理、数据淘汰、主备切换（高可用等功能）。

启动时MetaService、LocalService的配置由各自配置文件导入，默认配置文件在memcache的whl包config目录下 （通过 pip show memcache_hybrid 查看具体的路径）

#### 3.1 MetaService 配置文件

任选一个节点作为MetaService启动节点，将ock.mmc.meta_service_url、ock.mmc.meta_service.config_store_url中的IP修改为节点IP，保存为mmc-meta.conf

```ini
## IP 需要修改为启动节点IP
ock.mmc.meta_service_url = tcp://127.0.0.1:5000

## IP 需要修改为启动节点IP
ock.mmc.meta_service.config_store_url = tcp://127.0.0.1:6000

## HTTP service url
ock.mmc.meta_service.metrics_url = http://127.0.0.1:8000

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## For more configuration options and advanced parameters, please refer to the official documentation: [MemCache Configuration Guide](https://gitcode.com/Ascend/memcache/blob/master/doc/memcache_config.md)
```

#### 3.2 LocalService 配置文件

在每个节点上准备mmc-local.conf配置文件，将ock.mmc.meta_service_url、ock.mmc.local_service.config_store_url中的IP修改为MetaService启动节点IP，并根据实际情况配置内存容量

```ini
## 配置为MetaService启动节点IP
ock.mmc.meta_service_url = tcp://127.0.0.1:5000

## 配置为MetaService启动节点IP
ock.mmc.local_service.config_store_url = tcp://127.0.0.1:6000

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## The maximum supported rank count; once ranks are connected, no further modifications are allowed — a meta restart is required
ock.mmc.local_service.world_size = 256

 ## 卡侧ROCE网络传输为A2典型配置
ock.mmc.local_service.protocol = device_rdma

## 每卡申请的内存大小，可根据环境实际内存适当调整
ock.mmc.local_service.dram.size = 50GB

## For more configuration options and advanced parameters, please refer to the official documentation: [MemCache Configuration Guide](https://gitcode.com/Ascend/memcache/blob/master/doc/memcache_config.md)
```

### 4. 准备推理服务相关脚本

#### 4.1 在每个节点准备脚本`launch_online_dp.py`

```python
import argparse
import multiprocessing
import os
import subprocess
import sys


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dp-size", type=int, required=True, help="Data parallel size.")
    parser.add_argument("--tp-size", type=int, default=1, help="Tensor parallel size.")
    parser.add_argument("--dp-size-local", type=int, default=-1, help="Local data parallel size.")
    parser.add_argument("--dp-rank-start", type=int, default=0, help="Starting rank for data parallel.")
    parser.add_argument("--dp-address", type=str, required=True, help="IP address for data parallel master node.")
    parser.add_argument("--dp-rpc-port", type=str, default=12345, help="Port for data parallel master node.")
    parser.add_argument("--vllm-start-port", type=int, default=9000, help="Starting port for the engine.")
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
        process = multiprocessing.Process(target=run_command, args=(visible_devices, dp_rank, vllm_engine_port))
        processes.append(process)
        process.start()

    for process in processes:
        process.join()
```

#### 4.2 在每个节点上准备脚本`run_dp_template.sh`

需要修改的点包括：

- nic_name：使用ifconfig查询网卡名称
- 权重路径
- MMC_LOCAL_CONFIG_PATH：memcache localservice配置文件路径
- kv_port：对于每个实例必须唯一，如30000、30100等。

##### P0

```shell
unset ftp_proxy
unset https_proxy
unset http_proxy
rm -rf ~/ascend/log

nic_name="enp189s0f0"
local_ip=`hostname -I|awk -F " " '{print$1}'`

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
export TASK_QUEUE_ENABLE=1

export MMC_LOCAL_CONFIG_PATH=/home/hsy/config/mmc-local.conf
export HYBM_RDMA_SWAP_SPACE_SIZE=0
export PYTHONHASHSEED=0

vllm serve /home/weights/Eco-Tech/DeepSeek-V4-Flash-w8a8-mtp \
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
    --max-model-len 135000 \
    --max-num-batched-tokens 4096 \
    --max-num-seqs 16 \
    --block-size 128 \
    --enforce-eager \
    --async-scheduling \
    --no-disable-hybrid-kv-cache-manager \
    --enable-prefix-caching \
    --trust-remote-code \
    --gpu-memory-utilization 0.9 \
    --quantization ascend \
    --safetensors-load-strategy 'prefetch' \
    --model-loader-extra-config='{"enable_multithread_load": "true", "num_threads": 128}' \
    --tokenizer-mode deepseek_v4 \
    --tool-call-parser deepseek_v4 \
    --enable-auto-tool-choice \
    --reasoning-parser deepseek_v4 \
    --additional-config '{"enable_cpu_binding": true, "enable_shared_expert_dp": true}' \
    --speculative-config '{"num_speculative_tokens": 1, "method": "mtp","enforce_eager": true}' \
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
                            "dp_size": 2,
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
    }'
```

##### P1

```shell
unset ftp_proxy
unset https_proxy
unset http_proxy
rm -rf ~/ascend/log

nic_name="enp189s0f0"
local_ip=`hostname -I|awk -F " " '{print$1}'`

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
export TASK_QUEUE_ENABLE=1

export MMC_LOCAL_CONFIG_PATH=/home/hsy/config/mmc-local.conf
export HYBM_RDMA_SWAP_SPACE_SIZE=0
export PYTHONHASHSEED=0

vllm serve /home/weights/Eco-Tech/DeepSeek-V4-Flash-w8a8-mtp \
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
    --max-model-len 135000 \
    --max-num-batched-tokens 4096 \
    --max-num-seqs 16 \
    --block-size 128 \
    --enforce-eager \
    --async-scheduling \
    --no-disable-hybrid-kv-cache-manager \
    --enable-prefix-caching \
    --trust-remote-code \
    --gpu-memory-utilization 0.9 \
    --quantization ascend \
    --safetensors-load-strategy 'prefetch' \
    --model-loader-extra-config='{"enable_multithread_load": "true", "num_threads": 128}' \
    --tokenizer-mode deepseek_v4 \
    --tool-call-parser deepseek_v4 \
    --enable-auto-tool-choice \
    --reasoning-parser deepseek_v4 \
    --additional-config '{"enable_cpu_binding": true, "enable_shared_expert_dp": true}' \
    --speculative-config '{"num_speculative_tokens": 1, "method": "mtp","enforce_eager": true}' \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_producer",
        "kv_connector_extra_config": {
            "connectors": [
                {
                    "kv_connector": "MooncakeHybridConnector",
                    "kv_role": "kv_producer",
                    "kv_port": "30100",
                    "kv_connector_extra_config": {
                        "prefill": {
                            "dp_size": 2,
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
    }'
```

##### D0

```shell
unset ftp_proxy
unset https_proxy
unset http_proxy
rm -rf ~/ascend/log

nic_name="enp189s0f0"
local_ip=`hostname -I|awk -F " " '{print$1}'`

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

export MMC_LOCAL_CONFIG_PATH=/home/hsy/config/mmc-local.conf
export HYBM_RDMA_SWAP_SPACE_SIZE=0
export PYTHONHASHSEED=0

vllm serve /home/weights/Eco-Tech/DeepSeek-V4-Flash-w8a8-mtp \
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
    --max-model-len 135000 \
    --max-num-batched-tokens 60 \
    --max-num-seqs 30 \
    --async-scheduling \
    --block-size 128 \
    --no-disable-hybrid-kv-cache-manager \
    --no-enable-prefix-caching \
    --trust-remote-code \
    --gpu-memory-utilization 0.9 \
    --quantization ascend \
    --safetensors-load-strategy 'prefetch' \
    --model-loader-extra-config='{"enable_multithread_load": "true", "num_threads": 128}' \
    --tokenizer-mode deepseek_v4 \
    --tool-call-parser deepseek_v4 \
    --enable-auto-tool-choice \
    --reasoning-parser deepseek_v4 \
    --speculative-config '{"num_speculative_tokens": 1, "method": "mtp","enforce_eager": true}' \
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
                    "kv_port": "30400",
                    "kv_connector_extra_config": {
                        "prefill": {
                            "dp_size": 2,
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
    }'
```

##### D1

```shell
unset ftp_proxy
unset https_proxy
unset http_proxy
rm -rf ~/ascend/log

nic_name="enp189s0f0"
local_ip=`hostname -I|awk -F " " '{print$1}'`

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

export MMC_LOCAL_CONFIG_PATH=/home/hsy/config/mmc-local.conf
export HYBM_RDMA_SWAP_SPACE_SIZE=0
export PYTHONHASHSEED=0

vllm serve /home/weights/Eco-Tech/DeepSeek-V4-Flash-w8a8-mtp \
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
    --max-model-len 135000 \
    --max-num-batched-tokens 60 \
    --max-num-seqs 30 \
    --async-scheduling \
    --block-size 128 \
    --no-disable-hybrid-kv-cache-manager \
    --no-enable-prefix-caching \
    --trust-remote-code \
    --gpu-memory-utilization 0.9 \
    --quantization ascend \
    --safetensors-load-strategy 'prefetch' \
    --model-loader-extra-config='{"enable_multithread_load": "true", "num_threads": 128}' \
    --tokenizer-mode deepseek_v4 \
    --tool-call-parser deepseek_v4 \
    --enable-auto-tool-choice \
    --reasoning-parser deepseek_v4 \
    --speculative-config '{"num_speculative_tokens": 1, "method": "mtp","enforce_eager": true}' \
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
                    "kv_port": "30400",
                    "kv_connector_extra_config": {
                        "prefill": {
                            "dp_size": 2,
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
    }'
```

### 5. 拉起服务

#### 5.1 拉起MetaService元数据管理服务

在前面选中的MetaService启动节点通过如下脚本拉起MetaService元数据管理服务

```shell
# 修改为metaservice配置文件路径
export MMC_META_CONFIG_PATH=/home/hsy/config/mmc-meta.conf
python3 -c "from memcache_hybrid import MetaService; MetaService.main()"
```

#### 5.2 拉起VLLM推理服务

##### P0、P1

`--dp-address` 设置为每个实例所在节点的IP地址

```shell
python launch_online_dp.py --dp-size 2 --tp-size 4 --dp-size-local 2 --dp-rank-start 0 --dp-address <p_node_0_ip> --dp-rpc-port 12321 --vllm-start-port 7100

python launch_online_dp.py --dp-size 2 --tp-size 4 --dp-size-local 2 --dp-rank-start 0 --dp-address <p_node_1_ip> --dp-rpc-port 12321 --vllm-start-port 7100
```

##### D0、D1

`--dp-address` 设置为主D节点（D0）的IP地址

`--dp-rank-start` 从0开始每节点递增 `--dp-size-local`数量

```shell
python launch_online_dp.py --dp-size 16 --tp-size 1 --dp-size-local 8 --dp-rank-start 0 --dp-address <d_node_0_ip> --dp-rpc-port 12321 --vllm-start-port 7100

python launch_online_dp.py --dp-size 16 --tp-size 1 --dp-size-local 8 --dp-rank-start 8 --dp-address <d_node_0_ip> --dp-rpc-port 12321 --vllm-start-port 7100
```

#### 5.3 在任一节点拉起请求转发服务

vllm-ascend提供了两种具有不同请求路由行为的代理实现：

- **`load_balance_proxy_layerwise_server_example.py`**：请求首先路由到 D 节点，然后 D 节点根据需要转发到 P 节点。此代理设计用于与 MooncakeLayerwiseConnector 配合使用。[load_balance_proxy_layerwise_server_example.py](https://github.com/vllm-project/vllm-ascend/blob/main/examples/disaggregated_prefill_v1/load_balance_proxy_layerwise_server_example.py)
- **`load_balance_proxy_server_example.py`**：请求首先被路由到P节点，然后由P节点转发到D节点进行后续处理。该代理专为与MooncakeConnector配合使用而设计。[load_balance_proxy_server_example.py](https://github.com/vllm-project/vllm-ascend/blob/main/examples/disaggregated_prefill_v1/load_balance_proxy_server_example.py)

在任意节点拉起如下脚本：

```shell
python3 load_balance_proxy_server_example.py \
  --port 1999 \
  --host 127.0.0.1 \
  --prefiller-hosts \
    <p_node_0_ip> <p_node_0_ip> \
    <p_node_1_ip> <p_node_1_ip> \
  --prefiller-ports \
    7100 7101 \
    7100 7101 \
  --decoder-hosts \
    <d_node_0_ip> <d_node_0_ip> <d_node_0_ip> <d_node_0_ip> <d_node_0_ip> <d_node_0_ip> <d_node_0_ip> <d_node_0_ip> \
    <d_node_1_ip> <d_node_1_ip> <d_node_1_ip> <d_node_1_ip> <d_node_1_ip> <d_node_1_ip> <d_node_1_ip> <d_node_1_ip> \
  --decoder-ports \
    7100 7101 7102 7103 7104 7105 7106 7107 \
    7100 7101 7102 7103 7104 7105 7106 7107

```

## 测试方法

安装ais_bench_benchmark

```shell
pip3 install ais_bench_benchmark[full]
```

克隆 [rayn-zzz/aisbench_auto_tools_prefix](https://github.com/rayn-zzz/aisbench_auto_tools_prefix) 仓库并按照README说明配置config.py

测试命令参考：

```bash
python3 aisbench_test.py --input_len 65536 --output_len 1024 --data_num 250 --concurrency 50 --dataset_type prefix_cache --prefix_num 50 --repeat_rate 0.8 --seed $(date +%s)
```
