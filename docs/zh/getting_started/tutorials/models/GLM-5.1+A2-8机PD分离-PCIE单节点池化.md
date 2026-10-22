# GLM-5.1+A2-8机PD分离-PCIE单节点池化

## 概述

[MemCache](https://gitcode.com/Ascend/memcache) 是针对AI推理场景设计的高性能分布式KVCache缓存系统，针对昇腾超节点、服务器的实现了精细化性能调优，提供较好的High Availability能力，同时对接到vllm-ascend、sglang、mindie等开源推理框架。

**GLM-5.1** 是智谱最新旗舰模型，代码能力大大增强，**长程任务**显著提升，能够在单次任务中持续、自主地工作长达 8 小时，完成从规划、执行到迭代优化的完整闭环，交付工程级成果。使用MemCache作为vLLM KV Pool后端，可以实现高效的KV Cache存储和请求间复用，从而大幅优化长文本、高复用（prefix cache）场景下的TTFT和吞吐水平。

## 环境说明

【硬件规格】：A2 * 8

【模型权重】：GLM-5.1-w8a8c8-MTP

**注意：由于MemCache在A2上的优选方案（基于卡侧ROCE传输的多机池化）对NPU驱动固件和操作系统PAGESIZE有要求，部分客户环境不符合，本案例中采用了MemCache备选方案（基于PCIE传输的单机L2池化），消解了底软不兼容的问题。**

【NPU驱动固件】：无特殊要求

【OS PAGESIZE】：无特殊要求

## 测试结果

详细数据：

| 池化 | 总请求数 | 并发 | 前缀数 | 输入长度 | 输出长度 | 前缀重复率 | 前缀预热 | 实际命中率 | TTFT(ms) | TPOT(ms) |
| ---- | -------- | ---- | ------ | -------- | -------- | ---------- | -------- | ---------- | -------- | -------- |
| MMC  | 80       | 16   | 16     | 64K      | 1K       | 80%        | Y        | 48.65%     | 10634.9  | 35.3     |
| MMC  | 160      | 32   | 32     | 64K      | 1K       | 80%        | Y        | 48.675%    | 15038.9  | 41.4     |
| None | 80       | 16   | 16     | 64K      | 1K       | 80%        | Y        | 0.125%     | 29632.5  | 34.9     |
| None | 160      | 32   | 32     | 64K      | 1K       | 80%        | Y        | 0.05%      | 81990.6  | 35.2     |

测试工具：aisbench

## 部署

### 1. 拉取vllm-ascend官方镜像

```shell
docker pull quay.io/ascend/vllm-ascend:v0.23.0
```

启动容器增加 -v /dev/hugepages:/dev/hugepages 参数，可以使用大页内存提升memcache读写带宽

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

安装其他非正式发布版可以直接源码编译安装，参考 [官方编译文档](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/install_run.md) 即可。

### 3. 准备MemCache配置文件

MemCache由MetaService和LocalService两个组件构成，其中LocalService集成到推理框架中随推理进程一同启动，负责内存申请、数据读写和传输，MetaService作为独立进程启动，通过TCP与LocalService建立连接，负责元数据管理、数据淘汰、主备切换（高可用等功能）。

启动时MetaService、LocalService的配置由各自配置文件导入，默认配置文件在memcache的whl包config目录下。

**本案采用了每个节点独立作为一个内存池的方案，每个节点都需要拉起metaservice。**

#### MetaService 配置文件

在所有节点准备mmc-meta.conf

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

#### LocalService 配置文件

在每个节点上准备mmc-local.conf配置文件，根据实际情况调整内存容量

```ini
## 配置为MetaService启动节点IP
ock.mmc.meta_service_url = tcp://127.0.0.1:5000

## 配置为MetaService启动节点IP
ock.mmc.local_service.config_store_url = tcp://127.0.0.1:6000

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## The maximum supported rank count; once ranks are connected, no further modifications are allowed — a meta restart is required
ock.mmc.local_service.world_size = 256

## 使用基于共享内存的池化方案
ock.mmc.local_service.protocol = host_shm

## 单卡贡献的内存容量，D节点由于没有数据读写可以调整为1GB
ock.mmc.local_service.dram.size = 80GB

## For more configuration options and advanced parameters, please refer to the official documentation: [MemCache Configuration Guide](https://gitcode.com/Ascend/memcache/blob/master/doc/memcache_config.md)
```

### 4. 准备vllm启动脚本

#### 4.1 在每个节点上准备脚本 `launch_online_dp.py`

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
    parser.add_argument("--pp-size", type=int, default=1, help="Pipeline parallel size.")
    parser.add_argument("--dp-size-local", type=int, default=-1, help="Local data parallel size.")
    parser.add_argument("--dp-rank-start", type=int, default=0, help="Starting rank for data parallel.")
    parser.add_argument("--dp-address", type=str, required=True, help="IP address for data parallel master node.")
    parser.add_argument("--dp-rpc-port", type=str, default="12321", help="Port for data parallel master node.")
    parser.add_argument("--vllm-start-port", type=int, default=8000, help="Starting port for the engine.")
    return parser.parse_args()

args = parse_args()
dp_size = args.dp_size
tp_size = args.tp_size
pp_size = args.pp_size
dp_size_local = args.dp_size_local
if dp_size_local == -1:
    dp_size_local = dp_size
dp_rank_start = args.dp_rank_start
dp_address = args.dp_address
dp_rpc_port = args.dp_rpc_port
vllm_start_port = args.vllm_start_port
gpus_per_dp_rank = tp_size * pp_size

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
        str(pp_size),
    ]
    subprocess.run(command, check=True)

if __name__ == "__main__":
    template_path = "./run_dp_template.sh"
    if not os.path.exists(template_path):
        print(f"Template file {template_path} does not exist.")
        sys.exit(1)

    processes = []
    num_cards = dp_size_local * gpus_per_dp_rank

    for i in range(dp_size_local):
        dp_rank = dp_rank_start + i
        vllm_engine_port = vllm_start_port + i
        visible_devices = ",".join(str(x) for x in range(i * gpus_per_dp_rank, (i + 1) * gpus_per_dp_rank))
        process = multiprocessing.Process(
            target=run_command,
            args=(visible_devices, dp_rank, vllm_engine_port)
        )
        processes.append(process)
        process.start()

    for process in processes:
        process.join()
```

#### 4.2 在每个节点上准备脚本 run_dp_template.sh

##### P Node

```shell
export VLLM_RPC_TIMEOUT=3600000
export VLLM_EXECUTE_MODEL_TIMEOUT_SECONDS=30000
export HCCL_EXEC_TIMEOUT=204
export HCCL_CONNECT_TIMEOUT=120

nic_name="enp189s0f0"
local_ip=`hostname -I|awk -F " " '{print$1}'`

# 以下环境变量无需修改
export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=10
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export VLLM_ASCEND_ENABLE_MLAPO=1
export HCCL_BUFFSIZE=256
export TASK_QUEUE_ENABLE=1
export HCCL_OP_EXPANSION_MODE="AIV"
export VLLM_USE_V1=1
export ASCEND_RT_VISIBLE_DEVICES=$1
export LD_LIBRARY_PATH=/usr/local/python3.11.10/lib:/usr/local/lib:$LD_LIBRARY_PATH
#export LD_LIBRARY_PATH=/usr/local/python3.11.10/lib/python3.11/site-packages/mooncake:$LD_LIBRARY_PATH
export ASCEND_AGGREGATE_ENABLE=1
export ASCEND_TRANSPORT_PRINT=1
export VLLM_ASCEND_ENABLE_FLASHCOMM1=1

export PYTHONHASHSEED=0
export MMC_LOCAL_CONFIG_PATH="/mnt/share/h30028424/config/mmc-local-p.conf"
export HCCL_INTRA_ROCE_ENABLE=1

# vllm起服务配置
vllm serve /home/weights/Eco-Tech/GLM-5.1-w8a8c8-MTP/ \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --enable-prefix-caching \
    --seed 1024 \
    --enable-chunked-prefill \
    --served-model-name glm-5 \
    --async-scheduling \
    --max-model-len 141312 \
    --max-num-batched-tokens 8192 \
    --trust-remote-code \
    --max-num-seqs 256 \
    --gpu-memory-utilization 0.9 \
    --safetensors-load-strategy prefetch \
    --quantization ascend \
    --enforce-eager \
    --enable-auto-tool-choice \
    --tool-call-parser glm47 \
    --reasoning-parser glm45 \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_producer",
        "kv_load_failure_policy": "recompute",
        "kv_connector_extra_config": {
            "connectors": [
                {
                    "kv_connector": "MooncakeConnectorV1",
                    "kv_role": "kv_producer",
                    "kv_port": "30000",
                    "kv_connector_extra_config": {
                        "prefill": {
                            "dp_size": 4,
                            "tp_size": 8
                        },
                        "decode": {
                            "dp_size": 8,
                            "tp_size": 4
                        }
                    }
                },
                {
                    "kv_connector": "AscendStoreConnector",
                    "kv_role": "kv_producer",
                    "kv_connector_extra_config": {
                        "lookup_rpc_port": "0",
                        "backend": "memcache"
                    }
                }
            ]
        }
    }' \
    --additional-config \
    '{
        "multistream_overlap_shared_expert": true,
        "enable_dsa_cp": true,
        "enable_sparse_sfa_c8": true,
        "enable_sparse_li_c8": true,
        "fuse_muls_add": true,
        "recompute_scheduler_enable": false
    }' \
    --profiler-config \
    '{
        "profiler": "torch",
        "torch_profiler_dir": "/mnt/share/xxx/prof",
        "torch_profiler_with_stack": false
    }' \
    --speculative-config '{"num_speculative_tokens": 1, "method": "deepseek_mtp", "enforce_eager": true}'

```

##### D Node

```shell
export VLLM_RPC_TIMEOUT=3600000
export VLLM_EXECUTE_MODEL_TIMEOUT_SECONDS=30000
export HCCL_EXEC_TIMEOUT=204
export HCCL_CONNECT_TIMEOUT=120

nic_name="enp189s0f0"
local_ip=`hostname -I|awk -F " " '{print$1}'`

# 以下环境变量无需修改
export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=10
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export VLLM_ASCEND_ENABLE_MLAPO=1
export HCCL_BUFFSIZE=2560
export TASK_QUEUE_ENABLE=1
export HCCL_OP_EXPANSION_MODE="AIV"

export VLLM_USE_V1=1
export ASCEND_RT_VISIBLE_DEVICES=$1
export LD_LIBRARY_PATH=/usr/local/python3.11.10/lib:/usr/local/lib:$LD_LIBRARY_PATH
#export LD_LIBRARY_PATH=/usr/local/python3.11.10/lib/python3.11/site-packages/mooncake:$LD_LIBRARY_PATH

export PYTHONHASHSEED=0
export MMC_LOCAL_CONFIG_PATH="/mnt/share/h30028424/config/mmc-local-d.conf"
export HCCL_INTRA_ROCE_ENABLE=1

export ACL_OP_INIT_MODE=1

# vllm起服务配置
vllm serve /home/weights/Eco-Tech/GLM-5.1-w8a8c8-MTP/ \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-prefix-caching \
    --seed 1024 \
    --served-model-name glm-5 \
    --async-scheduling \
    --max-model-len 200000 \
    --max-num-batched-tokens 256 \
    --trust-remote-code \
    --max-num-seqs 128 \
    --gpu-memory-utilization 0.9 \
    --safetensors-load-strategy prefetch \
    --quantization ascend \
    --enable-auto-tool-choice \
    --tool-call-parser glm47 \
    --reasoning-parser glm45 \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_consumer",
        "kv_load_failure_policy": "recompute",
        "kv_connector_extra_config": {
            "connectors": [
                {
                    "kv_connector": "MooncakeConnectorV1",
                    "kv_role": "kv_consumer",
                    "kv_port": "30100",
                    "kv_connector_extra_config": {
                        "prefill": {
                            "dp_size": 4,
                            "tp_size": 8
                        },
                        "decode": {
                            "dp_size": 8,
                            "tp_size": 4
                        }
                    }
                },
                {
                    "kv_connector": "AscendStoreConnector",
                    "kv_role": "kv_consumer",
                    "kv_connector_extra_config": {
                        "lookup_rpc_port": "0",
                        "backend": "memcache"
                    }
                }
            ]
        }
    }' \
     --compilation-config \
    '{
        "cudagraph_mode": "FULL_DECODE_ONLY",
        "cudagraph_capture_sizes": [4,8,16,24,32,40,48,56,64,96,128,160,192,224,256,298,320,352,384]
    }' \
    --profiler-config \
    '{
        "profiler": "torch",
        "torch_profiler_dir": "/mnt/share/xxx/prof",
        "torch_profiler_with_stack": false
    }' \
    --additional-config \
    '{
        "multistream_overlap_shared_expert": true,
        "fuse_muls_add": true,
        "recompute_scheduler_enable": true,
        "enable_sparse_sfa_c8": true,
        "enable_sparse_li_c8": true
    }' \
    --speculative-config '{"num_speculative_tokens": 3, "method":"deepseek_mtp", "enforce_eager":true}'

```

**需要根据环境做相应修改的包括：**

- nic_name：本地网卡名称
- 权重路径
- MMC_LOCAL_CONFIG_PATH：MemCache LocalService 配置文件（mmc-local.conf）实际路径

#### 4.3 请求转发脚本

从vllm-ascend仓库中获取代理程序[load_balance_proxy_server_example.py](https://github.com/vllm-project/vllm-ascend/blob/main/examples/disaggregated_prefill_v1/load_balance_proxy_server_example.py)

准备如下请求转发脚本（需要修改对应ip、port为服务实际启动时所设置的ip、port），命名为start_proxy.sh

```shell
unset http_proxy
unset https_proxy

python load_balance_proxy_server_example.py \
    --port 9000 \
    --host 127.0.0.1 \
    --prefiller-hosts \
        <prefill_node_0_ip> \
        <prefill_node_1_ip> \
        <prefill_node_2_ip> \
        <prefill_node_3_ip> \
    --prefiller-ports \
        6700 \
        6700 \
        6700 \
        6700 \
    --decoder-hosts \
        <decode_node_0_ip> \
        <decode_node_0_ip> \
        <decode_node_1_ip> \
        <decode_node_1_ip> \
        <decode_node_2_ip> \
        <decode_node_2_ip> \
        <decode_node_3_ip> \
        <decode_node_3_ip> \
    --decoder-ports \
        6721 6722 \
        6721 6722 \
        6721 6722 \
        6721 6722
```

### 5. 拉起服务

#### 5.1 在各个节点配置大页内存

先在宿主机上通过如下命令查询大页配置，获取Hugepagesize。示例如下：

```shell
[root@node-106 memcache]# grep -i huge /proc/meminfo
AnonHugePages:   8689664 kB
ShmemHugePages:        0 kB
FileHugePages:         0 kB
HugePages_Total:   200000
HugePages_Free:    200000
HugePages_Rsvd:        0
HugePages_Surp:        0
Hugepagesize:       2048 kB
Hugetlb:        409600000 kB
```

这里 Hugepagesize：2048 kB 表示系统大页大小为2M。

然后计算需要配置的大页数量，方法如下：

```text
每台环境所需大页数量 = 单卡内存量 * 服务使用卡数 / 大页大小
```

例如，每卡配置80GB DRAM内存，使用8卡拉起服务，大页大小为2MB，则总共需要大页数量为 `80GB * 8 / 2MB = 327680`。

最后通过如下命令配置大页数量（假设配置327680个大页）

```shell
echo 327680 > /proc/sys/vm/nr_hugepages
```

如果是非root用户可使用如下命令

```shell
echo 327680 | sudo tee /proc/sys/vm/nr_hugepages
```

#### 5.2 在各个节点拉起MetaService元数据管理服务

在各个节点拉起MetaService元数据管理服务，配置文件路径修改为实际路径

```shell
export MMC_META_CONFIG_PATH=/xxx/mmc-meta.conf
python3 -c "from memcache_hybrid import MetaService; MetaService.main()"
```

#### 5.3 在各个节点拉起推理服务

**P0~P4**

```shell
nohup python launch_online_dp.py --dp-size 4 --tp-size 8 --dp-size-local 1 --dp-rank-start 0 --dp-address <prefill_node_0_ip> --dp-rpc-port 63521 --vllm-start-port 6700 > p0.log 2>&1 &

nohup python launch_online_dp.py --dp-size 4 --tp-size 8 --dp-size-local 1 --dp-rank-start 1 --dp-address <prefill_node_0_ip> --dp-rpc-port 63521 --vllm-start-port 6700 > p1.log 2>&1 &

nohup python launch_online_dp.py --dp-size 4 --tp-size 8 --dp-size-local 1 --dp-rank-start 2 --dp-address <prefill_node_0_ip> --dp-rpc-port 63521 --vllm-start-port 6700 > p2.log 2>&1 &

nohup python launch_online_dp.py --dp-size 4 --tp-size 8 --dp-size-local 1 --dp-rank-start 3 --dp-address <prefill_node_0_ip> --dp-rpc-port 63521 --vllm-start-port 6700 > p3.log 2>&1 &
```

**D0~D4**

```shell
nohup python launch_online_dp.py --dp-size 8 --tp-size 4 --dp-size-local 2 --dp-rank-start 0 --dp-address <decode_node_0_ip> --dp-rpc-port 63523 --vllm-start-port 6721 > d0.log 2>&1 &

nohup python launch_online_dp.py --dp-size 8 --tp-size 4 --dp-size-local 2 --dp-rank-start 2 --dp-address <decode_node_0_ip> --dp-rpc-port 63523 --vllm-start-port 6721 > d1.log 2>&1 &

nohup python launch_online_dp.py --dp-size 8 --tp-size 4 --dp-size-local 2 --dp-rank-start 4 --dp-address <decode_node_0_ip> --dp-rpc-port 63523 --vllm-start-port 6721 > d2.log 2>&1 &

nohup python launch_online_dp.py --dp-size 8 --tp-size 4 --dp-size-local 2 --dp-rank-start 6 --dp-address <decode_node_0_ip> --dp-rpc-port 63523 --vllm-start-port 6721 > d3.log 2>&1 &
```

#### 5.4 在任一节点拉起请求转发脚本

```shell
bash start_proxy.sh
```

## 测试方法

1. 安装ais_bench_benchmark

    ```shell
    pip3 install ais_bench_benchmark[full]
    ```

2. 配置aisbench_auto_tools_prefix工具

    克隆 [rayn-zzz/aisbench_auto_tools_prefix](https://github.com/rayn-zzz/aisbench_auto_tools_prefix) 仓库并按照README说明配置config.py

    **注意**：本次测试中开启了前缀预热功能，需要配置POD_INFO字段，示例如下：

    ```python
    # 数据集文件夹路径，需可访问(请使用绝对路径)
    DATASET_PATH = "/home/hsy/aisbench_auto_tools_prefix/dataset"

    # aisbench 工作路径, 为 git clone aisbench 后得到的 benchmark 目录的绝对路径
    # 可通过命令 `pip show ais-bench-benchmark` 查询location
    WORK_PATH = "/usr/local/python3.12.13/lib/python3.12/site-packages"

    # 服务化配置的模型名称
    MODEL_NAME = "glm-5"
    # 模型权重路径, 用于读取 tokenizer
    MODEL_PATH = "/home/weights/Eco-Tech/GLM-5.1-w8a8c8-MTP"
    # 请求目的 IP
    HOST_IP = "127.0.0.1"
    # 请求目的端口
    HOST_PORT = "9000"

    ## 鉴权信息
    API_KEY = ""

    # 如果使用稳态测试请将该字段设置为 "stable_stage"
    DEFAULT_PERFORMANCE_TEST = "default_perf"

    # aisbench输出日志保存路径
    OUTPUT_DIR = "./outputs/default"

    # 各节点信息，格式为 ["{ip}:{port}"]
    # 用于查询vllm metrics计算各个dp域的prefix cache命中率，不配置默认为HOST_IP:HOST_PORT
    # PD分离场景请填写各个P节点的IP和对应dp域的port
    POD_INFO = ["192.168.41.142:6700", "192.168.41.144:6700", "192.168.41.145:6700", "192.168.41.146:6700"]
    ```

3. 通过如下命令启动测试

    ```shell
    python3 aisbench_test.py --input_len 65536 --output_len 1024 --data_num 160 --concurrency 32 --dataset_type prefix_cache --prefix_num 32 --repeat_rate 0.8 --seed $(date +%s) --prefix_test --dp 4
    ```

    注意：

    - `--seed $(date +%s)` 表示用时间当随机种子，保证每次测试的数据互相之间不干扰
    - `--prefix_test --dp 4` 表示启用前缀预热，每个前缀都预热到4个prefill的DP域上
