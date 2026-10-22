# GLM-4.7+A2双机混部

## 概述

[MemCache](https://gitcode.com/Ascend/memcache) 是针对AI推理场景设计的高性能分布式KVCache缓存系统，针对昇腾超节点、服务器的实现了精细化性能调优，提供较好的High Availability能力，同时对接到vllm-ascend、sglang、mindie等开源推理框架。

GLM-4.7 系列是智谱的高智能模型，面向 **Agentic Coding** 场景强化了编码能力、长程任务规划与工具协同，并在多个公开基准的当期榜单中取得开源模型中的出色表现。使用MemCache作为vLLM KV Pool后端，可以实现高效的KV Cache存储和请求间复用，从而大幅优化长文本、高复用（prefix cache）场景下的TTFT和吞吐水平。

## 环境说明

【硬件规格】Atlas 800T A2 双机16卡

<span style="color:#e60000;">注意：采用本文的部署方案必须满足以下底软版本要求</span>

【NPU驱动固件】最低要求为25.5.1 [下载地址](https://www.hiascend.com/hardware/firmware-drivers/old/community?product=4&model=32&cann=All&driver=Ascend+HDK+25.5.1)

## 部署

本文部署以 [GLM-4.7_vLLM-ascend](https://ai.gitcode.com/Ascend-SACT/GLM-4.7_vLLM-ascend/tree/main) 中提供的环境和脚本为基线

【CANN软件】CANN 8.5

【推理框架】vLLM

```text
torch                             2.9.0+cpu
torch_npu                         2.9.0
torchvision                       0.24.0
transformers                      4.57.6
triton-ascend                     3.2.0
vllm                              0.14.1+empty               /vllm-workspace/vllm
vllm_ascend                       0.14.0rc2.dev68+gf55ded242 /vllm-workspace/vllm-ascend
```

### 1. 安装MF

在线安装：

```shell
pip install memfabric_hybrid==1.1.5
pip install memcache_hybrid==1.1.5
```

如果因网络原因无法在线安装，可以在 [PyPI · The Python Package Index](https://pypi.org/) 下载安装包离线安装。

安装其他非正式发布版可以直接源码编译安装，参考官方文档即可

[memcache/doc/install_run.md](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/install_run.md)

### 2. 修改配置文件

MemCache由MetaService和LocalService两个组件构成，其中LocalService集成到推理框架中随推理进程一同启动，负责内存申请、数据读写和传输，MetaService作为独立进程启动，通过TCP与LocalService建立连接，负责元数据管理、数据淘汰、主备切换（高可用等功能）。

启动时MetaService、LocalService的配置由各自配置文件导入，默认配置文件在memcache的whl包config目录下

```text
/usr/local/lib/python3.11/site-packages/memcache_hybrid/config
```

#### MetaService 配置文件

任选一个节点作为MetaService启动节点，将配置中ock.mmc.meta_service_url、ock.mmc.meta_service.config_store_url修改为节点IP，保存为mmc-meta.conf

```ini
## IP 需要修改为启动节点IP
ock.mmc.meta_service_url = tcp://127.0.0.1:5000

## IP 需要修改为启动节点IP
ock.mmc.meta_service.config_store_url = tcp://127.0.0.1:6000

## HTTP service url
ock.mmc.meta_service.metrics_url = http://127.0.0.1:8000

## Log level: debug, info, warn, error
ock.mmc.log_level = info
```

#### LocalService 配置文件

在每个节点上准备mmc-local.conf配置文件，根据实际情况修改IP和内存容量

```ini
## 配置为MetaService启动节点IP
ock.mmc.meta_service_url = tcp://127.0.0.1:5000

## 配置为MetaService启动节点IP
ock.mmc.local_service.config_store_url = tcp://127.0.0.1:6000

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## The maximum supported rank count; once ranks are connected, no further modifications are allowed — a meta restart is required
ock.mmc.local_service.world_size = 256

## 使用卡侧RDMA网络传输协议 是A2集群典型配置
ock.mmc.local_service.protocol = device_rdma

## 每个推理进程提供80GB池化空间
ock.mmc.local_service.dram.size = 80GB
```

更多配置可以参考官方文档 <https://gitcode.com/Ascend/memcache/blob/master/docs/zh/memcache_config.md>

### 3. 准备vLLM服务启动脚本

node0

```shell
local_ip="61.47.16.106"
nic_name="enp189s0f0"

export HCCL_IF_IP=$local_ip

#export PYTHONPATH=/workspace/vllm:$PYTHONPATH
export PYTHONPATH=/vllm-workspace/vllm:$PYTHONPATH

export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name
export HCCL_BUFFSIZE=1024
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=10
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export VLLM_USE_V1=1
export HCCL_OP_EXPANSION_MODE=AIV
export VLLM_VERSION=0.14.1
export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export VLLM_ASCEND_ENABLE_TOPK_OPTIMIZE=1
export VLLM_ASCEND_BALANCE_SCHEDULING=1
export VLLM_ASCEND_ENABLE_FLASHCOMM1=1

export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2:$LD_PRELOAD

export VLLM_TORCH_PROFILER_DIR=/home
export VLLM_TORCH_PROFILER_WITH_STACK=0

export MMC_LOCAL_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-local.conf

vllm serve /data/GLM-4.7-w8a8/ \
    --host 0.0.0.0 \
    --port 8013 \
    --enable-expert-parallel \
    --tensor-parallel-size 8 \
    --data_parallel_size 2 \
    --data-parallel-size-local 1 \
    --data-parallel-address 61.47.16.106 \
    --data-parallel-rpc-port 13345  \
    --seed 1024 \
    --served-model-name GLM-4.7-w8a8 \
    --max-model-len 133120 \
    --max-num-batched-tokens 8192 \
    --max-num-seqs 32 \
    --async-scheduling \
    --quantization ascend \
    --trust-remote-code \
    --gpu-memory-utilization 0.9 \
    --speculative-config '{"num_speculative_tokens": 3, "model":"/data/GLM-4.7-w8a8/", "method":"mtp"}' \
    --compilation-config '{"cudagraph_capture_sizes": [1,4,8,16,32,64], "cudagraph_mode": "FULL_DECODE_ONLY"}' \
    --additional-config '{"ascend_compilation_config": {"fuse_qknorm_rope": true}, "ascend_fusion_config": {"fusion_ops_gmmswigluquant": false}, "multistream_overlap_shared_expert":"true", "enable_shared_expert_dp": "true"}' \
    --kv-transfer-config \
    '{
        "kv_connector": "AscendStoreConnector",
        "kv_role": "kv_both",
        "kv_connector_extra_config": {
            "backend": "memcache",
            "mooncake_rpc_port":"0"
        }
    }' 2>&1 | tee output.log

```

node1

```shell
local_ip="61.47.16.107"
nic_name="enp189s0f0"

export HCCL_IF_IP=$local_ip

#export PYTHONPATH=/workspace/vllm:$PYTHONPATH
export PYTHONPATH=/vllm-workspace/vllm:$PYTHONPATH

export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name
export HCCL_BUFFSIZE=1024
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=10
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export VLLM_USE_V1=1
export HCCL_OP_EXPANSION_MODE=AIV
export VLLM_VERSION=0.14.1
export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export VLLM_ASCEND_ENABLE_TOPK_OPTIMIZE=1
export VLLM_ASCEND_BALANCE_SCHEDULING=1
export VLLM_ASCEND_ENABLE_FLASHCOMM1=1

export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2:$LD_PRELOAD

export VLLM_TORCH_PROFILER_DIR=/home
export VLLM_TORCH_PROFILER_WITH_STACK=0

export MMC_LOCAL_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-local.conf

vllm serve /data/GLM-4.7-w8a8/ \
    --host 0.0.0.0 \
    --port 8013 \
    --headless \
    --enable-expert-parallel \
    --tensor-parallel-size 8 \
    --data_parallel_size 2 \
    --data-parallel-size-local 1 \
    --data-parallel-start-rank 1 \
    --data-parallel-address 61.47.16.106 \
    --data-parallel-rpc-port 13345  \
    --seed 1024 \
    --served-model-name GLM-4.7-w8a8 \
    --max-model-len 133120 \
    --max-num-batched-tokens 8192 \
    --max-num-seqs 32 \
    --async-scheduling \
    --quantization ascend \
    --trust-remote-code \
    --gpu-memory-utilization 0.9 \
    --speculative-config '{"num_speculative_tokens": 3, "model":"/data/GLM-4.7-w8a8/", "method":"mtp"}' \
    --compilation-config '{"cudagraph_capture_sizes": [1,4,8,16,32,64], "cudagraph_mode": "FULL_DECODE_ONLY"}' \
    --additional-config '{"ascend_compilation_config": {"fuse_qknorm_rope": true}, "ascend_fusion_config": {"fusion_ops_gmmswigluquant": false}, "multistream_overlap_shared_expert":"true", "enable_shared_expert_dp": "true"}' \
    --kv-transfer-config \
    '{
        "kv_connector": "AscendStoreConnector",
        "kv_role": "kv_both",
        "kv_connector_extra_config": {
            "backend": "memcache",
            "mooncake_rpc_port":"0"
        }
    }' 2>&1 | tee output.log

```

**需要根据环境做相应修改的包括：**

- local_ip：本机IP地址
- nic_name：本地网卡名称
- data-parallel-address：都写DP并行的主节点IP
- 权重路径

**相比基线脚本，启用MF时仅涉及以下两点变化**

1. 添加了环境变量导入MemCache LocalService配置文件路径（这里给的是whl包默认配置文件路径）

    ```shell
    export MMC_LOCAL_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-local.conf
    ```

2. 添加 kv-transfer-config 启动参数

    ```shell
    --kv-transfer-config \
        '{
            "kv_connector": "AscendStoreConnector",
            "kv_role": "kv_both",
            "kv_connector_extra_config": {
                "backend": "memcache"
            }
        }'
    ```

### 4. 拉起服务

首先需要拉起MetaService元数据管理服务

```shell
export MMC_META_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-meta.conf
python3 -c "from memcache_hybrid import MetaService; MetaService.main()"
```

然后使用脚本拉起推理服务，等待启动完成即可

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
            "value": 10240
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
    --model /data/GLM-4.7-w8a8/ \
    --served-model-name GLM-4.7-w8a8 \
    --input-file generate_multi_turn.json \
    --num-clients 8 \                        # 并发数
    --max-active-conversations 16 \
    --no-early-stop \
    --request-timeout-sec 600 \
    -u http://<ip>:<port>                    # 推理服务启动的ip port
```

**A2双机实测结果**

| MF      | 轮数 | 每轮输入 | 每轮输出 | 并发 | 总会话数 | HBM命中率 | DRAM命中率 | mean ttft (ms) | mean tpot (ms) | 总耗时 (s) |
| ------- | ---- | -------- | -------- | ---- | -------- | --------- | ---------- | -------------- | -------------- | ---------- |
| disable | 20   | 10240    | 1024     | 8    | 16       | 17.8%     | /          | 18320.94       | 126.96         | 2835.736   |
| enable  | 20   | 10240    | 1024     | 8    | 16       | 10.25%    | 55.75%     | 7646.07        | 65.63          | 1451.111   |
| disable | 20   | 9216     | 1024     | 8    | 20       | 24.15%    | /          | 15152.60       | 109.57         | 2647.894   |
| enable  | 20   | 9216     | 1024     | 8    | 20       | 21.25%    | 52.9%      | 6182.19        | 60.95          | 1411.294   |
| disable | 40   | 4096     | 1024     | 8    | 20       | 27.85%    | /          | 15090.55       | 107.69         | 5130.660   |
| enable  | 40   | 4096     | 1024     | 8    | 20       | 23.1%     | 60.85%     | 4253.99        | 48.70          | 2210.721   |

平均TTFT下降幅度可达60%~70%

## 指标观察

收集vLLM日志中的如下指标数据，可以绘制出运行指标变化趋势

```text
GPU KV cache usage: XX%, Prefix cache hit rate: XX%, External prefix cache hit rate: XX%
```

![metrics-trend.png](https://raw.gitcode.com/user-images/assets/7672915/b3ec1e88-c776-453e-9fec-8909a7321f54/metrics-trend.png 'metrics-trend.png')

分析：

第一阶段，随序列长度逐渐提升，GPU KV Cache使用率和HBM命中率同步提升

第二阶段，GPU KV Cache使用率达到90%以上，Device侧缓存的prefix cache被释放，此时表现为 HBM命中率开始下滑（如果开启了内存池此时DRAM命中率开始提升）

第三阶段，GPU KV Cache使用率维持在90%以上，HBM命中率接近0，DRAM命中率持续提升

外部命中率越高，性能收益越明显。
