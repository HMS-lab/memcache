# GLM-5.1+A3-4机PD分离

## 概述

[MemCache](https://gitcode.com/Ascend/memcache) 是针对AI推理场景设计的高性能分布式KVCache缓存系统，针对昇腾超节点、服务器的实现了精细化性能调优，提供较好的High Availability能力，同时对接到vllm-ascend、sglang、mindie等开源推理框架。

**GLM-5.1** 是智谱最新旗舰模型，代码能力大大增强，**长程任务**显著提升，能够在单次任务中持续、自主地工作长达 8 小时，完成从规划、执行到迭代优化的完整闭环，交付工程级成果。使用MemCache作为vLLM KV Pool后端，可以实现高效的KV Cache存储和请求间复用，从而大幅优化长文本、高复用（prefix cache）场景下的TTFT和吞吐水平。

## 测试结果总结

**多轮会话数据集测试**

|     | MF      | 轮数 | 每轮输入 | 每轮输出 | 并发 | 总会话数 | mean ttft (ms)  | mean tpot (ms) | 总耗时 (s)  | HBM命中率 | DRAM命中率 |
|-----|---------|----|------|------|----|------|-----------------|----------------|----------|--------|---------|
| 第一组 | disable | 20 | 9216 | 1024 | 16 | 16   | 48166.97        | 8.61           | 594.513  | 2.8%   | /       |
| 第一组 | enable  | 20 | 9216 | 1024 | 16 | 16   | 27450.59 (↓43%) | 9.92           | 377.931  | 1.6%   | 68.6%   |
| 第二组 | disable | 40 | 4096 | 1024 | 16 | 16   | 37498.54        | 15.53          | 1070.425 | 4.0%   | /       |
| 第二组 | enable  | 40 | 4096 | 1024 | 16 | 16   | 17744.16 (↓52%) | 17.33          | 710.006  | 4.2%   | 82.3%   |
| 第三组 | disable | 20 | 9216 | 1024 | 30 | 30   | 90407.57        | 7.56           | 1048.888 | 0.1%   | /       |
| 第三组 | enable  | 20 | 9216 | 1024 | 30 | 30   | 41756.93 (↓54%) | 8.91           | 519.047  | 0.2%   | 69.2%   |

**90%固定前缀重复率测试**

在4机A3集群（每机8×910C，每张910C为双910B合封）上，基于GLM5.1 W8A8量化模型、PD分离架构（2 Prefill \* 4DP \* 8TP + 2 Decode \* 8DP \* 4TP），对MemCache前缀缓存进行了4组场景的性能测试。

| 场景 | Input | 并发（实际CC） | TTFT无缓存(ms) | TTFT有缓存(ms) | TTFT降幅 |
|----|-------|----------|-------------|-------------|--------|
| 1  | 64k   | 16       | 71723       | 14407       | 79.91% |
| 2  | 128k  | 13       | 83286       | 27883       | 66.52% |
| 3  | 64k   | 24       | 77366       | 8931        | 88.45% |
| 4  | 128k  | 21       | 120392      | 38678       | 67.87% |

**结论**

1. **TTFT大幅下降**：所有场景TTFT大幅下降 ~80.6%，首Token响应时间从分钟级降至秒级
2. **实际并发显著提升**：缓存命中后模型侧实际并发从6提升至24，服务承载能力翻倍

## 环境说明

【硬件规格】Atlas 900 A3 SuperPoD 四机64卡

【部署配置】2P2D （Prefill TP8 DP4，Decode TP4 DP8）

<span style="color:#e60000;">注意：采用本文的部署方案必须满足以下底软要求</span>

【操作系统内核版本】>=5.10.0

【NPU驱动固件】>=25.5.1

【L1 1520 CC】>=1.5.0

## 部署

本文采用的部署方案基于 [vllm-ascend部署GLM-5.1官方指导文档](https://docs.vllm.ai/projects/vllm-ascend-cn/zh-cn/latest/tutorials/models/GLM5.html) 修改。

### 1. 拉取vllm-ascend官方镜像

```shell
docker pull quay.io/ascend/vllm-ascend:v0.18.0-a3
```

<span style="color:#e60000;">**vllm-ascend版本可以高于此版本，此版本只是案例推出时的vllm-ascend版本，非强制要求，vllm-ascend版本可按需选择合适的release版本测试**</span>

启动容器的命令参考 [vllm-ascend部署GLM-5.1官方指导文档](https://docs.vllm.ai/projects/vllm-ascend-cn/zh-cn/latest/tutorials/models/GLM5.html)。

### 2. 安装MF

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

安装其他非正式发布版可以直接源码编译安装，参考官方文档即可

[memcache/doc/install_run.md](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/install_run.md)

### 3. 修改配置文件

MemCache由MetaService和LocalService两个组件构成，其中LocalService负责内存申请、数据读写和传输，MetaService作为独立进程启动，通过TCP与LocalService建立连接，负责元数据管理、数据淘汰、主备切换（高可用等功能）。

启动时MetaService、LocalService的配置由各自配置文件导入，默认配置文件在memcache的whl包config目录下 （通过 pip show memcache_hybrid 查看具体的路径）

```text
/usr/local/lib/python3.11/site-packages/memcache_hybrid/config
```

**本文基于A3上典型配置，采用了分离部署模式，每个节点将独立启动一个LocalService负责申请内存，而跟随推理进程启动的LocalService只支持读写数据不提供内存。
方案的详细介绍请参考[MemCache分离部署文档](../../MemCache+vLLM+A3独立部署案例.md)。**

#### 准备 MetaService 配置文件

任选一个节点作为MetaService启动节点，将配置中ock.mmc.meta_service_url、ock.mmc.meta_service.config_store_url修改为节点IP，保存为mmc-meta.conf

```ini
## IP 需要修改为MetaService启动节点IP
ock.mmc.meta_service_url = tcp://127.0.0.1:5000

## IP 需要修改为MetaService启动节点IP
ock.mmc.meta_service.config_store_url = tcp://127.0.0.1:6000

## HTTP service url
ock.mmc.meta_service.metrics_url = http://127.0.0.1:8000

## Log level: debug, info, warn, error
ock.mmc.log_level = info
```

#### 准备不提供内存的 LocalService 配置文件

将如下配置中的IP替换为MetaService节点IP，在每个节点上保存为mmc-local.conf

```ini
## 配置为MetaService启动节点IP
ock.mmc.meta_service_url = tcp://127.0.0.1:5000

## 配置为MetaService启动节点IP
ock.mmc.local_service.config_store_url = tcp://127.0.0.1:6000

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## The maximum supported rank count; once ranks are connected, no further modifications are allowed — a meta restart is required
ock.mmc.local_service.world_size = 256

## 超节点内sdma传输为A3典型配置
ock.mmc.local_service.protocol = device_sdma

## 跟随推理进程启动的LocalService不申请内存
ock.mmc.local_service.dram.size = 0GB

ock.mmc.local_service.max.dram.size = 1024GB
```

#### 准备提供内存的 LocalService 配置文件

运行[mem_scan.py](https://gitcode.com/Ascend/memfabric_hybrid/blob/master/src/smem/python/memfabric_hybrid/memfabric_hybrid/mem_scan.py)脚本扫描可用的物理内存：

`python3 mem_scan.py`

![image.png](https://raw.gitcode.com/user-images/assets/7672916/348c0854-4efb-4803-8222-f6bf19589fa1/image.png 'image.png')

通常2T内存的A3服务器最多有600+GB内存可供MemCache使用。如果扫描结果较少，可以通过下列命令清掉文件缓存

<span style="color:#e60000;">！！！注意！！！此命令为清缓存命令，会导致权重等缓存需要从文件系统重新加载，再次拉起vllm框架会变慢，请熟悉其背后代表的含义判断是否使用。</span>

```shell
echo 3 > /proc/sys/vm/drop_caches
```

如仍然较少，可能是内存碎片导致的，需要重启环境恢复。

假设在上一步中扫描得知环境上至少有600GB可用内存，将如下配置中IP替换为MetaService节点IP，在每个节点上保存为mmc-local-standalone.conf

```ini
## 配置为MetaService启动节点IP
ock.mmc.meta_service_url = tcp://127.0.0.1:5000

## 配置为MetaService启动节点IP
ock.mmc.local_service.config_store_url = tcp://127.0.0.1:6000

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## The maximum supported rank count; once ranks are connected, no further modifications are allowed — a meta restart is required
ock.mmc.local_service.world_size = 256

## 超节点内sdma传输为A3典型配置
ock.mmc.local_service.protocol = device_sdma

## 独立启动的LocalService提供600GB内存
ock.mmc.local_service.dram.size = 600GB

ock.mmc.local_service.max.dram.size = 1024GB
```

更多配置可以参考官方文档[MemCache Configs](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/memcache_config.md)

### 4. 准备推理服务启动相关脚本

#### 4.1 服务拉起辅助脚本

在每个节点上准备脚本 `launch_online_dp.py`：

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

#### 4.2 vLLM启动脚本

在每个节点上准备脚本 `run_dp_template.sh`

##### P0

```shell
nic_name="enp194s0f0" # change to your own nic name
local_ip="141.61.39.233" # change to your own ip

export HCCL_OP_EXPANSION_MODE="AIV"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export OMP_PROC_BIND=false
export OMP_NUM_THREADS=1
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=256

export ASCEND_AGGREGATE_ENABLE=1
export ASCEND_TRANSPORT_PRINT=1
export ACL_OP_INIT_MODE=1
export ASCEND_A3_ENABLE=1
# Timeout (in seconds) for automatically releasing the prefiller’s KV cache for a particular request.
export VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480

export ASCEND_RT_VISIBLE_DEVICES=$1
export VLLM_ASCEND_ENABLE_FLASHCOMM1=1

export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

export MMC_LOCAL_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-local.conf
export PYTHONHASHSEED=0

vllm serve /home/weights/GLM5.1-W8A8 \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --speculative-config '{"num_speculative_tokens": 3, "method":"deepseek_mtp"}' \
    --profiler-config '{"profiler": "torch", "torch_profiler_dir": "./vllm_profile", "torch_profiler_with_stack": false}' \
    --seed 1024 \
    --served-model-name glm-5 \
    --max-model-len 200704 \
    --additional-config '{"fuse_muls_add": true, "multistream_overlap_shared_expert": true, "ascend_compilation_config": {"enable_npugraph_ex": true}, "layer_sharding": ["q_b_proj", "o_proj"]}' \
    --max-num-batched-tokens 4096 \
    --trust-remote-code \
    --max-num-seqs 64 \
    --async-scheduling \
    --enable-chunked-prefill \
    --quantization ascend \
    --gpu-memory-utilization 0.95 \
    --enforce-eager \
    --enable-auto-tool-choice \
    --tool-call-parser glm47 \
    --reasoning-parser glm45 \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_producer",
        "engine_id": "0",
        "kv_connector_extra_config": {
            "connectors": [
                {
                    "kv_connector": "MooncakeConnectorV1",
                    "kv_role": "kv_producer",
                    "kv_port": "30000",
                    "kv_connector_extra_config": {
                        "use_ascend_direct": true,
                        "prefill": {"dp_size": 4, "tp_size": 8},
                        "decode": {"dp_size": 8, "tp_size": 4}
                    }
                },
                {
                    "kv_connector": "AscendStoreConnector",
                    "kv_role": "kv_producer",
                    "kv_connector_extra_config": {
                        "backend": "memcache",
                        "lookup_rpc_port": "0"
                    }
                }
            ]
        }
    }' 2>&1 | tee output.log
```

##### P1

```shell
nic_name="enp48s3u1u1" # change to your own nic name
local_ip="141.61.39.235" # change to your own ip

export HCCL_OP_EXPANSION_MODE="AIV"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export OMP_PROC_BIND=false
export OMP_NUM_THREADS=1
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=256

export ASCEND_AGGREGATE_ENABLE=1
export ASCEND_TRANSPORT_PRINT=1
export ACL_OP_INIT_MODE=1
export ASCEND_A3_ENABLE=1
# Timeout (in seconds) for automatically releasing the prefiller’s KV cache for a particular request.
export VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480

export ASCEND_RT_VISIBLE_DEVICES=$1
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True

export VLLM_ASCEND_ENABLE_FLASHCOMM1=1

export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

export MMC_LOCAL_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-local.conf
export PYTHONHASHSEED=0

vllm serve /home/weights/GLM5.1-W8A8 \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --speculative-config '{"num_speculative_tokens": 3, "method":"deepseek_mtp"}' \
    --profiler-config '{"profiler": "torch", "torch_profiler_dir": "./vllm_profile", "torch_profiler_with_stack": false}' \
    --seed 1024 \
    --served-model-name glm-5 \
    --max-model-len 200704 \
    --additional-config '{"fuse_muls_add": true, "multistream_overlap_shared_expert": true, "ascend_compilation_config": {"enable_npugraph_ex": true}, "layer_sharding": ["q_b_proj", "o_proj"]}' \
    --max-num-batched-tokens 4096 \
    --trust-remote-code \
    --max-num-seqs 64 \
    --async-scheduling \
    --enable-chunked-prefill \
    --gpu-memory-utilization 0.95 \
    --quantization ascend \
    --enforce-eager \
    --enable-auto-tool-choice \
    --tool-call-parser glm47 \
    --reasoning-parser glm45 \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_producer",
        "engine_id": "0",
        "kv_connector_extra_config": {
            "connectors": [
                {
                    "kv_connector": "MooncakeConnectorV1",
                    "kv_role": "kv_producer",
                    "kv_port": "30000",
                    "kv_connector_extra_config": {
                        "use_ascend_direct": true,
                        "prefill": {"dp_size": 4, "tp_size": 8},
                        "decode": {"dp_size": 8, "tp_size": 4}
                    }
                },
                {
                    "kv_connector": "AscendStoreConnector",
                    "kv_role": "kv_producer",
                    "kv_connector_extra_config": {
                        "backend": "memcache",
                        "lookup_rpc_port": "1"
                    }
                }
            ]
        }
    }' 2>&1 | tee output.log
```

##### D0

```shell
nic_name="enx9c69d302197d" # change to your own nic name
local_ip="141.61.39.237" # change to your own ip

export HCCL_OP_EXPANSION_MODE="AIV"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

#Mooncake
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=1

export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=256


export ASCEND_AGGREGATE_ENABLE=1
export ASCEND_TRANSPORT_PRINT=1
export ACL_OP_INIT_MODE=1
export ASCEND_A3_ENABLE=1
# Timeout (in seconds) for automatically releasing the prefiller’s KV cache for a particular request.
export VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480

export TASK_QUEUE_ENABLE=1

export ASCEND_RT_VISIBLE_DEVICES=$1

export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export VLLM_ASCEND_ENABLE_MLAPO=1
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

export MMC_LOCAL_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-local.conf
export PYTHONHASHSEED=0

vllm serve /home/weights/GLM5.1-W8A8 \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --speculative-config '{"num_speculative_tokens": 3,  "method":"deepseek_mtp"}' \
    --profiler-config '{"profiler": "torch", "torch_profiler_dir": "./vllm_profile", "torch_profiler_with_stack": false}' \
    --seed 1024 \
    --served-model-name glm-5 \
    --max-model-len 200704 \
    --max-num-batched-tokens 64 \
    --compilation-config '{"cudagraph_mode": "FULL_DECODE_ONLY", "cudagraph_capture_sizes": [4,8,12,16,20,24,28,32]}' \
    --additional-config '{"fuse_muls_add": true, "multistream_overlap_shared_expert": true, "ascend_compilation_config": {"enable_npugraph_ex": true}}' \
    --trust-remote-code \
    --max-num-seqs 16 \
    --gpu-memory-utilization 0.92 \
    --async-scheduling \
    --quantization ascend \
    --enable-auto-tool-choice \
    --tool-call-parser glm47 \
    --reasoning-parser glm45 \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_consumer",
        "engine_id": "1",
        "kv_connector_extra_config": {
            "connectors": [
                {
                    "kv_connector": "MooncakeConnectorV1",
                    "kv_role": "kv_consumer",
                    "kv_port": "30100",
                    "kv_connector_extra_config": {
                        "use_ascend_direct": true,
                        "prefill": {"dp_size": 4, "tp_size": 8},
                        "decode": {"dp_size": 8, "tp_size": 4}
                    }
                },
                {
                    "kv_connector": "AscendStoreConnector",
                    "kv_role": "kv_consumer",
                    "kv_connector_extra_config": {
                        "backend": "memcache",
                        "lookup_rpc_port": "2"
                    }
                }
            ]
        }
    }' 2>&1 | tee output.log
```

##### D1

```shell
nic_name="enx9c69d3020bab" # change to your own nic name
local_ip="141.61.39.238" # change to your own ip

export HCCL_OP_EXPANSION_MODE="AIV"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

#Mooncake
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=1

export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=256

export ASCEND_AGGREGATE_ENABLE=1
export ASCEND_TRANSPORT_PRINT=1
export ACL_OP_INIT_MODE=1
export ASCEND_A3_ENABLE=1
# Timeout (in seconds) for automatically releasing the prefiller’s KV cache for a particular request.
export VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480

export TASK_QUEUE_ENABLE=1

export ASCEND_RT_VISIBLE_DEVICES=$1

export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export VLLM_ASCEND_ENABLE_MLAPO=1
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

export MMC_LOCAL_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-local.conf
export PYTHONHASHSEED=0

vllm serve /home/weights/GLM5.1-W8A8 \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --speculative-config '{"num_speculative_tokens": 3,  "method":"deepseek_mtp"}' \
    --profiler-config '{"profiler": "torch", "torch_profiler_dir": "./vllm_profile", "torch_profiler_with_stack": false}' \
    --seed 1024 \
    --served-model-name glm-5 \
    --max-model-len 200704 \
    --max-num-batched-tokens 64 \
    --compilation-config '{"cudagraph_mode": "FULL_DECODE_ONLY", "cudagraph_capture_sizes": [4,8,12,16,20,24,28,32]}' \
    --additional-config '{"fuse_muls_add": true, "multistream_overlap_shared_expert": true, "ascend_compilation_config": {"enable_npugraph_ex": true}}' \
    --trust-remote-code \
    --max-num-seqs 16 \
    --gpu-memory-utilization 0.92 \
    --async-scheduling \
    --quantization ascend \
    --enable-auto-tool-choice \
    --tool-call-parser glm47 \
    --reasoning-parser glm45 \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_consumer",
        "engine_id": "1",
        "kv_connector_extra_config": {
            "connectors": [
                {
                    "kv_connector": "MooncakeConnectorV1",
                    "kv_role": "kv_consumer",
                    "kv_port": "30100",
                    "kv_connector_extra_config": {
                        "use_ascend_direct": true,
                        "prefill": {"dp_size": 4, "tp_size": 8},
                        "decode": {"dp_size": 8, "tp_size": 4}
                    }
                },
                {
                    "kv_connector": "AscendStoreConnector",
                    "kv_role": "kv_consumer",
                    "kv_connector_extra_config": {
                        "backend": "memcache",
                        "lookup_rpc_port": "3"
                    }
                }
            ]
        }
    }' 2>&1 | tee output.log
```

**需要根据环境做相应修改的包括：**

- local_ip：本机IP地址
- nic_name：本地网卡名称
- 权重路径

**相比基线脚本，启用MF时仅涉及以下两点变化**

1. 添加环境变量

    ```shell
    # 导入MemCache LocalService配置文件路径（这里给的是whl包默认配置文件路径）
    export MMC_LOCAL_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-local.conf
    # 设置Python哈子函数随机种子，保证不同实例接收相同请求后生成相同key
    export PYTHONHASHSEED=0
    ```

2. kv-transfer-config从仅使用MooncakeConnectorV1变为同时使用MooncakeConnectorV1+AscendStoreConnector

   - MooncakeConnectorV1负责每个请求P=>D传输kvcache
   - AscendStoreConnector负责调用memcache读写接口实现请求间prefix cache复用

#### 4.3 请求转发脚本

从vllm-ascend仓库中获取代理程序[load_balance_proxy_server_example.py](https://github.com/vllm-project/vllm-ascend/blob/main/examples/disaggregated_prefill_v1/load_balance_proxy_server_example.py)

使用如下脚本启动转发（需要修改对应ip、port为服务实际启动时所设置的ip、port）

```shell
unset http_proxy
unset https_proxy

node_p0_ip=141.61.39.233
node_p1_ip=141.61.39.235
node_d0_ip=141.61.39.237
node_d1_ip=141.61.39.238

python load_balance_proxy_server_example.py \
    --port 8000 \
    --host 0.0.0.0 \
    --prefiller-hosts \
        $node_p0_ip \
        $node_p1_ip \
    --prefiller-ports \
        6700 \
        6700 \
    --decoder-hosts \
        $node_d0_ip \
        $node_d0_ip \
        $node_d0_ip \
        $node_d0_ip \
        $node_d1_ip \
        $node_d1_ip \
        $node_d1_ip \
        $node_d1_ip \
    --decoder-ports \
        6721 6722 6723 6724 \
        6721 6722 6723 6724
```

### 5. 拉起服务

#### 5.1 拉起MetaService服务

在之前选定的节点拉起MetaService元数据管理服务

```shell
export MMC_META_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-meta.conf
python3 -c "from memcache_hybrid import MetaService; MetaService.main()"
```

#### 5.2 拉起MemCache独立进程

在每个节点上准备如下脚本，命名为memcache_standalone.py。

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

在每个节点通过如下命令拉起MemCache独立进程

```shell
export MMC_LOCAL_CONFIG_PATH="mmc-local-standalone.conf"
python3 memcache_standalone.py
```

**等待所有节点都启动成功（看到打印"Successfully initialized memcache. "）后再进行下一步**

#### 5.3 拉起推理服务

在每个节点上通过以下命令拉起推理服务

##### P0

```shell
python launch_online_dp.py --dp-size 4 --tp-size 8 --dp-size-local 2 --dp-rank-start 0 --dp-address 141.61.39.233 --dp-rpc-port 10521 --vllm-start-port 6700
```

##### P1

```shell
python launch_online_dp.py --dp-size 4 --tp-size 8 --dp-size-local 2 --dp-rank-start 2 --dp-address 141.61.39.233 --dp-rpc-port 10521 --vllm-start-port 6700
```

##### D0

```shell
python launch_online_dp.py --dp-size 8 --tp-size 4 --dp-size-local 4 --dp-rank-start 0 --dp-address 141.61.39.237 --dp-rpc-port 10523 --vllm-start-port 6721
```

##### D1

```shell
python launch_online_dp.py --dp-size 8 --tp-size 4 --dp-size-local 4 --dp-rank-start 4 --dp-address 141.61.39.237 --dp-rpc-port 10523 --vllm-start-port 6721
```

最后拉起请求转发脚本

```shell
bash start_proxy.sh
```

## 测试

推荐使用vLLM官方多轮会话测试工具 [vllm/benchmarks/multi_turn/README.md at main · vllm-project/vllm](https://github.com/vllm-project/vllm/blob/main/benchmarks/multi_turn/README.md)

在generate_multi_turn.json中可以配置对话数量、轮数、每轮输入输出等关键参数，这里以下面第一组数据为例

```json
{
    "filetype": "generate_conversations",
    "num_conversations": 16,
    "text_files": ["pg1184.txt"],
    "print_stats": false,
    "prompt_input": {
        "num_turns": {
            "distribution": "constant",
            "value": 20
        },
        "prefix_num_tokens": {
            "distribution": "constant",
            "value": 10240
        },
        "num_tokens": {
            "distribution": "constant",
            "value": 9216
        }
    },
    "prompt_output": {
        "num_tokens": {
            "distribution": "constant",
            "value": 1024
        }
    }
}

```

使用如下命令启动测试

```shell
python benchmark_serving_multi_turn.py \
    --model /home/weights/GLM5.1-W8A8 \
    --served-model-name glm-5 \
    --input-file generate_multi_turn.json \
    --num-clients 16 \                       # 并发数
    --max-active-conversations 16 \
    --no-early-stop \
    --request-timeout-sec 600 \
    -u http://<ip>:<port>                    # proxy脚本启动的ip port
```

**测试结果**

|        | MF      | 轮数 | 每轮输入 | 每轮输出 | 并发 | 总会话数 | mean ttft (ms)  | mean tpot (ms) | 总耗时 (s) | HBM命中率 | DRAM命中率 |
| ------ | ------- | ---- | -------- | -------- | ---- | -------- | --------------- | -------------- | ---------- | --------- | ---------- |
| 第一组 | disable | 20   | 9216     | 1024     | 16   | 16       | 48166.97        | 8.61           | 594.513    | 2.8%      | /          |
| 第一组 | enable  | 20   | 9216     | 1024     | 16   | 16       | 27450.59 (↓43%) | 9.92           | 377.931    | 1.6%      | 68.6%      |
| 第二组 | disable | 40   | 4096     | 1024     | 16   | 16       | 37498.54        | 15.53          | 1070.425   | 4.0%      | /          |
| 第二组 | enable  | 40   | 4096     | 1024     | 16   | 16       | 17744.16 (↓52%) | 17.33          | 710.006    | 4.2%      | 82.3%      |
| 第三组 | disable | 20   | 9216     | 1024     | 30   | 30       | 90407.57        | 7.56           | 1048.888   | 0.1%      | /          |
| 第三组 | enable  | 20   | 9216     | 1024     | 30   | 30       | 41756.93 (↓54%) | 8.91           | 519.047    | 0.2%      | 69.2%      |
