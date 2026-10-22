# GLM-5.2+A3-4机PD分离

## 概述

[MemCache](https://gitcode.com/Ascend/memcache) 是针对AI推理场景设计的高性能分布式KVCache缓存系统，针对昇腾超节点、服务器的实现了精细化性能调优，提供较好的High Availability能力，同时对接到vllm-ascend、sglang、mindie等开源推理框架。

GLM-5.2 是面向长任务时代的旗舰模型。支持真正可用的1M上下文，实测可承载项目级工程上下文，长程任务执行更稳定、工程规范遵循更可靠，开发场景成功率进一步提升。一次任务即可完成“从需求到多端可部署产物”的完整开发链路。使用MemCache作为vLLM KV Pool后端，可以实现高效的KV Cache存储和请求间复用，从而大幅优化长文本、高复用（prefix cache）场景下的TTFT和吞吐水平。

## 部署准备

### 前置要求

【硬件规格】：Atlas 800I A3 * 4

【驱动固件】：>=25.5.1

【L1 1520 CC】>=1.5.0

【OS内核版本】>=5.10

### 环境准备

GLM-5.2-w4a8c8权重下载地址：
[*https://modelscope.cn/models/Eco-Tech/GLM-5.2-w4a8c8*](https://modelscope.cn/models/Eco-Tech/GLM-5.2-w4a8c8)

拉取vllm镜像：
``docker pull quay.io/ascend/vllm-ascend:v0.23.0rc1``
以下保存为start-docker.sh脚本

```bash
#!/bin/bash
IMAGES_ID="$1"
NAME="$2"

# 检查参数数量（需要2个：镜像ID和容器名）
if [ $# -ne 2 ]; then
    echo "error: 需要传入2个参数，格式：$0 <镜像ID> <容器名>"
    exit 1
fi

# 检查镜像是否存在
if ! docker images --format "{{.ID}}" | grep -q "^${IMAGES_ID:0:12}$"; then
    echo "error: 镜像ID $IMAGES_ID 不存在"
    exit 1
fi

# 启动容器（变量加引号，避免特殊字符问题）
docker run --name "${NAME}" -it -d --net=host --shm-size=500g \
    --privileged=true \
    -w /home \
    --device=/dev/davinci_manager \
    --device=/dev/hisi_hdc \
    --device=/dev/devmm_svm \
    --entrypoint=bash \
    -v /usr/local/Ascend/driver:/usr/local/Ascend/driver \
    -v /usr/local/dcmi:/usr/local/dcmi \
    -v /usr/local/bin/npu-smi:/usr/local/bin/npu-smi \
    -v /etc/ascend_install.info:/etc/ascend_install.info \
    -v /usr/local/sbin:/usr/local/sbin \
    -v /etc/hccn.conf:/etc/hccn.conf \
    -v /home:/home \
    -v /mnt:/mnt \
    -v /tmp:/tmp \
    -v /data:/data \
    -v /usr/share/zoneinfo/Asia/Shanghai:/etc/localtime \
    -e http_proxy="$http_proxy" \
    -e https_proxy="$https_proxy" \
    "${IMAGES_ID}"
```

执行以下命令创建容器
`bash start-docker.sh 611962961cde glm5.2-memcache`
执行以下命令进入创建完成的容器
`docker exec -it glm5.2-memcache bash`

## MemCache

### 安装

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

### 修改MemCache配置文件

**步骤一：获取配置文件路径**

启动时MetaService、LocalService的配置由各自配置文件导入，默认配置文件在memcache的whl包config目录下。

```bash
# 获取python路径
which python
/usr/local/python3.12.13/bin/python
# 则配置文件路径为：
/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-meta.conf
/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf
```

**步骤二：修改配置文件**
假设计划在P0节点（IP记为**P0_node_ip**）拉起MetaService，需将P0节点mmc-meta.conf、mmc-local.conf配置文件做如下修改，然后将mmc-local.conf配置文件拷贝到另外P1、D0、D1节点相同位置（4个节点配置一致）。

修改MetaService 配置文件：
`vim /usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-meta.conf`
进行如下内容修改：

```ini
## Meta service start-up url
ock.mmc.meta_service_url = tcp://<P0_node_ip>:5000
## IP 需要修改为MetaService启动节点IP

## Config store url
ock.mmc.meta_service.config_store_url = tcp://<P0_node_ip>:6000
## IP 需要修改为MetaService启动节点IP

## HTTP service url
ock.mmc.meta_service.metrics_url = http://<P0_node_ip>:8000
## IP 需要修改为MetaService启动节点IP

## Log level: debug, info, warn, error
ock.mmc.log_level = info
```

注意：以上3处 127.0.0.1 需改为启动meta_service服务节点IP。
**修改LocalService 配置文件：**
`vim /usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf`
进行如下内容修改：

```ini
## Meta service start-up url
## Keep consistent with the same name configuration in mmc-meta.conf
ock.mmc.meta_service_url = tcp://<P0_node_ip>:5000
## 配置为MetaService启动节点IP

## Config store url
## Keep consistent with the same name configuration in mmc-meta.conf
ock.mmc.local_service.config_store_url = tcp://<P0_node_ip>:6000
## 配置为MetaService启动节点IP

## Log level: debug, info, warn, error
ock.mmc.log_level = info

## The maximum supported rank count; once ranks are connected, no further modifications are allowed — a meta restart is required
ock.mmc.local_service.world_size = 256

## Data transfer protocol options
ock.mmc.local_service.protocol = device_sdma
## 超节点内sdma传输为A3典型配置，A2设备使用device_rdma

## DRAM space usage
ock.mmc.local_service.dram.size = 0GB
## 由独立进程进行空间占用，此处设置为0GB

ock.mmc.local_service.max.dram.size = 1024GB
## 新增配置
```

注意：其中2处 127.0.0.1 需改为启动meta_service服务节点IP

### 所有节点清理缓存空间

**若部署服务前已reboot重启过节点，则可跳过此步骤。**

系统缓存空间中可能占用了部分memcache可用内存地址，执行free -h命令可查看系统缓存空间占用情况，需执行以下命令清理缓存，给memcache分配更多的可用空间。注意：若出现之前拉起模型服务速度快，清理缓存后拉起模型服务的速度变慢属于正常现象，因为缓存中的模型权重被清空，需重新加载，时间变长。

echo 3 > /proc/sys/vm/drop_caches

清理完成后，可执行free -h查看buff/cache值是否降低。

或通过脚本扫描可用空间，请参考以下链接
[memfabric-hybrid - Wiki - AtomGit](https://gitcode.com/Ascend/memfabric_hybrid/wiki/FAQ.md#2-%E8%B0%83%E7%94%A8halmemcreate%E8%BF%94%E5%9B%9E%E9%94%99%E8%AF%AF%E7%A0%81-6)

### 启动主进程

注意：本操作只需在MetaService启动节点上执行，启动一个MetaService服务即可，不需指定具体目录执行。

```bash
export MMC_META_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-meta.conf
#启动mmc-meta
nohup python3 -c "from memcache_hybrid import MetaService; MetaService.main()" >> /data/mmc-log/run_meta_service.log 2>&1 &
```

### 所有节点启动独立进程占用memcache可用空间

**注意：四台节点均执行以下步骤，每台节点拉起一个独立进程。**

**步骤一**：**复制一份mmc-local配置文件**​，用于memcache独立进程启动使用，并修改其申请的内存容量。

```bash
mkdir -p /data/glm-5.2-start
cp /usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf /data/deploy_glm5.2_dp32tp1/mmc-local.conf
```

**步骤二**：对拷贝后的mmc-local.conf做出如下修改：

```bash
vim /data/deploy_glm5.2_dp32tp1/mmc-local.conf
```

修改配置：

```bash
ock.mmc.local_service.dram.size = 600GB
```

说明：每个节点根据实际情况进行设置，最终可用kv pool空间为4台A3节点设置的dram.size空间总和，如4台节点都设置为600GB，则最终可用池化空间为600GB * 4 = 2400GB。每台节点占用空间可以不同。
**步骤三**：新建独立进程启动脚本。

`vim /data/deploy_glm5.2_dp32tp1/memcache.py`

脚本内容：

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

**步骤四**：启动独立进程。

```bash
export MMC_LOCAL_CONFIG_PATH=/data/deploy_glm5.2_dp32tp1/mmc-local.conf
#启动独立进程
nohup python3 /data/deploy_glm5.2_dp32tp1/memcache.py >> /data/mmc-log/run_mmc-local.log 2>&1 &
```

**注意**：独立进程启动脚本执行完后，不要着急拉起GLM-5.2的vllm serve服务，查看/data/mmc-log/run_mmc-local.log日志，待4台节点均出现如下日志时，说明4台节点独立进程占用内存正常。若出现内存分配失败的报错，则需要调低ock.mmc.local_service.dram.size配置，重新拉起独立进程，直到4个独立进程均成功拉起。

```text
Successfully initialized memcache.
```

也可以通过 `http://<P0_node_ip>:8000/metrics/summary` 页面快速访问确认总kv pool大小

## 部署GLM-5.2高性能服务

### 所有的节点容器中准备脚本launch_online_dp.py

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

### 在所有的节点容器中准备部署脚本run_dp_template.sh

P节点

```bash
nic_name="bond0" # change to your own nic name
local_ip="100.xx.xx.xx" # change to your own ip

export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export HCCL_OP_EXPANSION_MODE="AIV"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export OMP_PROC_BIND=false
export OMP_NUM_THREADS=1
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=400

export ACL_OP_INIT_MODE=1
export ASCEND_A3_ENABLE=1

export ASCEND_RT_VISIBLE_DEVICES=$1
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

export VLLM_ASCEND_ENABLE_FLASHCOMM1=1

#memcache
export PYTHONHASHSEED=0
export VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480
export MMC_LOCAL_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf
export VLLM_EXECUTE_MODEL_TIMEOUT_SECONDS=1800

vllm serve /data/GLM-5.2-w4a8c8 \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --speculative-config '{"num_speculative_tokens":1, "method":"deepseek_mtp","enforce_eager":true}' \
    --seed 1024 \
    --served-model-name glm-52 \
    --max-model-len 210000 \
    --additional-config '{"recompute_scheduler_enable" : false,"multistream_overlap_shared_expert": true, "enable_dsa_cp":true,"enable_sparse_sfa_c8": false, "enable_sparse_li_c8": true,"c8_enable_reshape_optim":false}' \
    --max-num-batched-tokens 8192 \
    --trust-remote-code \
    --max-num-seqs 64 \
    --quantization ascend \
    --gpu-memory-utilization 0.92 \
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
                    "kv_port": "40000",
                    "kv_connector_extra_config": {
                        "use_ascend_direct": true,
                        "prefill": {"dp_size": 4, "tp_size": 8},
                        "decode": {"dp_size": 32, "tp_size": 1}
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
    }'
```

D节点

```bash
nic_name="eth-bond4" # change to your own nic name
local_ip="100.xx.xx.xx" # change to your own ip

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
export TASK_QUEUE_ENABLE=1
export ASCEND_RT_VISIBLE_DEVICES=$1
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib
export VLLM_ASCEND_ENABLE_FUSED_MC2=1

#memcache
export PYTHONHASHSEED=0
export VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480
export MMC_LOCAL_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf
export VLLM_EXECUTE_MODEL_TIMEOUT_SECONDS=1800

vllm serve /data/GLM-5.2-w4a8c8 \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --seed 1024 \
    --served-model-name glm-52 \
    --max-model-len 210000 \
    --max-num-batched-tokens 164 \
    --compilation-config '{"cudagraph_mode":"FULL_DECODE_ONLY"}' \
    --speculative-config '{"num_speculative_tokens": 5,  "method":"deepseek_mtp","enforce_eager":true}' \
    --additional-config '{"recompute_scheduler_enable":true,"multistream_overlap_shared_expert":true,"enable_sparse_sfa_c8": false, "enable_sparse_li_c8": true}' \
    --trust-remote-code \
    --max-num-seqs 32 \
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
                    "kv_port": "40100",
                    "kv_connector_extra_config": {
                        "use_ascend_direct": true,
                        "prefill": {"dp_size": 4, "tp_size": 8},
                        "decode": {"dp_size": 32, "tp_size": 1}
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
    }'
```

### 依次在每个节点上启动推理服务

Prefill1 Node0启动，创建p0-run.sh文件运行，需要修改高亮的地方为实际的ip和端口：

```bash
nohup python launch_online_dp.py \
  --dp-size 4 --tp-size 8 --dp-size-local 2 \
  --dp-rank-start 0 --dp-address <P0_node_ip> \
  --dp-rpc-port 10521 --vllm-start-port 6700 \
  >> launch_dp_output.log 2>&1 &
```

Prefill1 Node1启动，创建p1-run.sh文件运行，需要修改高亮的：

```bash
nohup python launch_online_dp.py \
  --dp-size 4 --tp-size 8 --dp-size-local 2 \
  --dp-rank-start 2 --dp-address <P0_node_ip> \
  --dp-rpc-port 10521 --vllm-start-port 6700 \
  >> launch_dp_output.log 2>&1 &
```

Decode1 Node0启动，创建d0-run.sh文件运行，需要修改高亮的：

```bash
nohup python launch_online_dp.py \
  --dp-size 32 --tp-size 1 --dp-size-local 16 \
  --dp-rank-start 0 --dp-address <D0_node_ip> \
  --dp-rpc-port 10521 --vllm-start-port 6700 \
  >> launch_dp_output.log 2>&1 &
```

Decode1 Node1启动，创建d1-run.sh文件运行，需要修改高亮的：

```bash
nohup python launch_online_dp.py \
  --dp-size 32 --tp-size 1 --dp-size-local 16 \
  --dp-rank-start 16 --dp-address <D0_node_ip> \
  --dp-rpc-port 10521 --vllm-start-port 6700 \
  >> launch_dp_output.log 2>&1 &
```

### 通过proxy对外提供服务ip和端口

脚本从容器中的/vllm-workspace路径下的版本文件中中获取：

```bash
root@localhost:/vllm-workspace# find -name load_balance_proxy_server_example.py
./vllm-ascend/examples/disaggregated_prefill_v1/load_balance_proxy_server_example.py
```

创建start_proxy.sh脚本，拉起代理转发服务，脚本内容如下：

```bash
#!/bin/bash

# 取消代理设置
unset http_proxy
unset https_proxy

# 后台运行程序
nohup python load_balance_proxy_server_example.py \
    --port 8088 \
    --host <P0_node_ip> \
    --prefiller-hosts \
       "<P0_node_ip>" \
       "<P0_node_ip>" \
       "<P1_node_ip>" \
       "<P1_node_ip>" \
    --prefiller-ports \
       6700 6701\
       6700 6701\
    --decoder-hosts \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D0_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
      "<D1_node_ip>" \
    --decoder-ports \
      6700 6701 6702 6703 6704 6705 6706 6707 6708 6709 6710 6711 6712 6713 6714 6715 \
      6700 6701 6702 6703 6704 6705 6706 6707 6708 6709 6710 6711 6712 6713 6714 6715 \
    >> start_proxy.log 2>&1 &
```

创建好脚本后，执行以下命令拉起代理服务：
`bash start_proxy.sh`

## 部署GLM-5.2长序列（1M）服务

脚本launch_online_dp.py参考前一节高性能服务

### 所有节点部署脚本配置

P0节点配置

```bash
nic_name="bond0"
local_ip="<P0_node_ip>"
node_p0_ip="<P0_node_ip>"
# Prefiller node 0: data_parallel_start_rank=0, server_role_args="--api-server-count 1"
# Prefiller node 1: data_parallel_start_rank=2, server_role_args="--headless"
data_parallel_start_rank=0
server_role_args="--api-server-count 1"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export VLLM_ASCEND_ENABLE_NZ=1
export HCCL_OP_EXPANSION_MODE="AIV"
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=20
export HCCL_BUFFSIZE=768
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export VLLM_WORKER_MULTIPROC_METHOD=spawn
export TASK_QUEUE_ENABLE=1

#memcache
export PYTHONHASHSEED=0
export VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480
export MMC_LOCAL_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf

vllm serve /data/GLM-5.2-w4a8c8 \
  --seed 1024 \
  --host 0.0.0.0 \
  --port 9081 \
  --served-model-name glm-52 \
  --max-model-len 1040000 \
  --max-num-batched-tokens 16384 \
  --gpu-memory-utilization 0.78 \
  ${server_role_args} \
  --max-num-seqs 8 \
  --data-parallel-size 4 \
  --data-parallel-size-local 2 \
  --data-parallel-start-rank $data_parallel_start_rank \
  --data-parallel-address $node_p0_ip \
  --data-parallel-rpc-port 10521 \
  --pipeline-parallel-size 1 \
  --tensor-parallel-size 8 \
  --prefill-context-parallel-size 1 \
  --decode-context-parallel-size 8 \
  --cp-kv-cache-interleave-size 128 \
  --enforce-eager \
  --additional-config '{"enable_flashcomm1": true, "enable_dsa_cp": true, "ascend_compilation_config": {"enable_npugraph_ex": true, "enable_static_kernel": false}, "fuse_muls_add": true, "multistream_overlap_shared_expert": true, "enable_mc2_hierarchy_comm": false, "enable_sparse_sfa_c8": false, "enable_sparse_li_c8": true, "enable_cpu_binding": true, "recompute_scheduler_enable": true}' \
  --speculative-config '{"num_speculative_tokens": 1, "method": "deepseek_mtp", "enforce_eager": true}' \
  --quantization ascend \
  --enable-expert-parallel \
  --safetensors-load-strategy prefetch \
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
                    "kv_port": "40000",
                    "kv_connector_extra_config": {
                        "use_ascend_direct": true,
                        "prefill": {"dp_size": 4, "tp_size": 8},
                        "decode": {"dp_size": 4, "tp_size": 8}
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
    }'
```

P1节点配置

```bash
nic_name="bond0"
local_ip="<P1_node_ip>"
node_p0_ip="<P0_node_ip>"
# Prefiller node 0: data_parallel_start_rank=0, server_role_args="--api-server-count 1"
# Prefiller node 1: data_parallel_start_rank=2, server_role_args="--headless"
data_parallel_start_rank=2
server_role_args="--headless"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export VLLM_ASCEND_ENABLE_NZ=1
export HCCL_OP_EXPANSION_MODE="AIV"
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=20
export HCCL_BUFFSIZE=768
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export VLLM_WORKER_MULTIPROC_METHOD=spawn
export TASK_QUEUE_ENABLE=1

#memcache
export PYTHONHASHSEED=0
export VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480
export MMC_LOCAL_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf

vllm serve /data/GLM-5.2-w4a8c8 \
  --seed 1024 \
  --host 0.0.0.0 \
  --port 9081 \
  --served-model-name glm-52 \
  --max-model-len 1040000 \
  --max-num-batched-tokens 16384 \
  --gpu-memory-utilization 0.78 \
  ${server_role_args} \
  --max-num-seqs 8 \
  --data-parallel-size 4 \
  --data-parallel-size-local 2 \
  --data-parallel-start-rank $data_parallel_start_rank \
  --data-parallel-address $node_p0_ip \
  --data-parallel-rpc-port 10521 \
  --pipeline-parallel-size 1 \
  --tensor-parallel-size 8 \
  --prefill-context-parallel-size 1 \
  --decode-context-parallel-size 8 \
  --cp-kv-cache-interleave-size 128 \
  --enforce-eager \
  --additional-config '{"enable_flashcomm1": true, "enable_dsa_cp": true, "ascend_compilation_config": {"enable_npugraph_ex": true, "enable_static_kernel": false}, "fuse_muls_add": true, "multistream_overlap_shared_expert": true, "enable_mc2_hierarchy_comm": false, "enable_sparse_sfa_c8": false, "enable_sparse_li_c8": true, "enable_cpu_binding": true, "recompute_scheduler_enable": true}' \
  --speculative-config '{"num_speculative_tokens": 1, "method": "deepseek_mtp", "enforce_eager": true}' \
  --quantization ascend \
  --enable-expert-parallel \
  --safetensors-load-strategy prefetch \
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
                    "kv_port": "40000",
                    "kv_connector_extra_config": {
                        "use_ascend_direct": true,
                        "prefill": {"dp_size": 4, "tp_size": 8},
                        "decode": {"dp_size": 4, "tp_size": 8}
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
    }'
```

D0节点配置

```bash
nic_name="eth-bond4"
local_ip="<D0_node_ip>"
node_d0_ip="<D0_node_ip>"
# Decoder node 0: data_parallel_start_rank=0, server_role_args="--api-server-count 1"
# Decoder node 1: data_parallel_start_rank=2, server_role_args="--headless"
data_parallel_start_rank=0
server_role_args="--api-server-count 1"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export VLLM_ASCEND_ENABLE_NZ=1
export HCCL_OP_EXPANSION_MODE="AIV"
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=20
export HCCL_BUFFSIZE=768
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export VLLM_WORKER_MULTIPROC_METHOD=spawn

#memcache
export PYTHONHASHSEED=0
export VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480
export MMC_LOCAL_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf
export TASK_QUEUE_ENABLE=1

vllm serve /data/GLM-5.2-w4a8c8 \
  --seed 1024 \
  --host 0.0.0.0 \
  --port 9900 \
  --served-model-name glm-52 \
  --max-model-len 1040000 \
  --max-num-batched-tokens 128 \
  --gpu-memory-utilization 0.93 \
  ${server_role_args} \
  --max-num-seqs 32 \
  --data-parallel-size 4 \
  --data-parallel-size-local 2 \
  --data-parallel-start-rank $data_parallel_start_rank \
  --data-parallel-address $node_d0_ip \
  --data-parallel-rpc-port 16600 \
  --pipeline-parallel-size 1 \
  --tensor-parallel-size 8 \
  --prefill-context-parallel-size 1 \
  --decode-context-parallel-size 8 \
  --cp-kv-cache-interleave-size 128 \
  --compilation-config '{"cudagraph_mode": "FULL_DECODE_ONLY"}' \
  --additional-config '{"enable_flashcomm1": false, "enable_dsa_cp": false, "ascend_compilation_config": {"enable_npugraph_ex": true, "enable_static_kernel": false}, "fuse_muls_add": true, "multistream_overlap_shared_expert": true, "enable_mc2_hierarchy_comm": false, "enable_sparse_sfa_c8": false, "enable_sparse_li_c8": true, "enable_cpu_binding": true, "recompute_scheduler_enable": true}' \
  --speculative-config '{"num_speculative_tokens": 3, "method": "deepseek_mtp", "enforce_eager": true}' \
  --quantization ascend \
  --enable-expert-parallel \
  --safetensors-load-strategy prefetch \
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
                    "kv_port": "40100",
                    "kv_connector_extra_config": {
                        "use_ascend_direct": true,
                        "prefill": {"dp_size": 4, "tp_size": 8},
                        "decode": {"dp_size": 4, "tp_size": 8}
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
    }'
```

D1节点配置

```bash
nic_name="eth-bond4"
local_ip="<D1_node_ip>"
node_d0_ip="<D0_node_ip>"
# Decoder node 0: data_parallel_start_rank=0, server_role_args="--api-server-count 1"
# Decoder node 1: data_parallel_start_rank=2, server_role_args="--headless"
data_parallel_start_rank=2
server_role_args="--headless"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export VLLM_ASCEND_ENABLE_NZ=1
export HCCL_OP_EXPANSION_MODE="AIV"
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=20
export HCCL_BUFFSIZE=768
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export VLLM_WORKER_MULTIPROC_METHOD=spawn

#memcache
export PYTHONHASHSEED=0
export VLLM_MOONCAKE_ABORT_REQUEST_TIMEOUT=480
export MMC_LOCAL_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf
export TASK_QUEUE_ENABLE=1

vllm serve /data/GLM-5.2-w4a8c8 \
  --seed 1024 \
  --host 0.0.0.0 \
  --port 9900 \
  --served-model-name glm-52 \
  --max-model-len 1040000 \
  --max-num-batched-tokens 128 \
  --gpu-memory-utilization 0.93 \
  ${server_role_args} \
  --max-num-seqs 32 \
  --data-parallel-size 4 \
  --data-parallel-size-local 2 \
  --data-parallel-start-rank $data_parallel_start_rank \
  --data-parallel-address $node_d0_ip \
  --data-parallel-rpc-port 16600 \
  --pipeline-parallel-size 1 \
  --tensor-parallel-size 8 \
  --prefill-context-parallel-size 1 \
  --decode-context-parallel-size 8 \
  --cp-kv-cache-interleave-size 128 \
  --compilation-config '{"cudagraph_mode": "FULL_DECODE_ONLY"}' \
  --additional-config '{"enable_flashcomm1": false, "enable_dsa_cp": false, "ascend_compilation_config": {"enable_npugraph_ex": true, "enable_static_kernel": false}, "fuse_muls_add": true, "multistream_overlap_shared_expert": true, "enable_mc2_hierarchy_comm": false, "enable_sparse_sfa_c8": false, "enable_sparse_li_c8": true, "enable_cpu_binding": true, "recompute_scheduler_enable": true}' \
  --speculative-config '{"num_speculative_tokens": 3, "method": "deepseek_mtp", "enforce_eager": true}' \
  --quantization ascend \
  --enable-expert-parallel \
  --safetensors-load-strategy prefetch \
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
                    "kv_port": "40100",
                    "kv_connector_extra_config": {
                        "use_ascend_direct": true,
                        "prefill": {"dp_size": 4, "tp_size": 8},
                        "decode": {"dp_size": 4, "tp_size": 8}
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
    }'
```

### 依次在每个节点上启动推理服务

将前一节的四个启动脚本创建成对应的**-run.sh文件，四台机器各自执行：
`nohup bash p0-run.sh >> launch_dp_output.log 2>&1 &`
`nohup bash p1-run.sh >> launch_dp_output.log 2>&1 &`
`nohup bash d0-run.sh >> launch_dp_output.log 2>&1 &`
`nohup bash d1-run.sh >> launch_dp_output.log 2>&1 &`

### 通过proxy对外提供服务ip和端口

创建start_proxy.sh脚本，拉起代理转发服务，脚本内容如下：

```bash
#!/bin/bash

# 取消代理设置
unset http_proxy
unset https_proxy

# 后台运行程序
nohup python /vllm-workspace/vllm-ascend/examples/disaggregated_prefill_v1/load_balance_proxy_server_example.py \
  --host 0.0.0.0 \
  --port 8088 \
  --prefiller-hosts <P0_node_ip> \
  --prefiller-ports 9081 \
  --decoder-hosts <D0_node_ip> \
  --decoder-ports 9900 \
    >> start_proxy.log 2>&1 &
```

创建好脚本后，执行以下命令拉起代理服务：
`bash start_proxy.sh`

## 性能测试

在四机A3集群上，基于GLM5.2-w4a8c8量化模型、PD分离架构（2节点组成Prefill + 2节点 Decode）基于高性能服务配置，对MemCache前缀缓存进行了4组场景的性能测试。有缓存测试数据的前缀数量种类是并发数量一半，90%前缀重复，--dp 4预热前缀测试，模拟多用户多种前缀AI Coding场景。主要结果如下

| 场景         | Input Len | 实际 并发 | 有无缓存   | TTFT(ms) | TTFT降幅 | 输出吞吐(token/s) | 输出吞吐提升 |
| ------------ | --------- | --------- | ---------- | -------- | -------- | ----------------- | ------------ |
| 64K × 20CC   | 65536     | 17.71     | 无缓存     | 23,405   | /        | 436.3             | /            |
| 64K × 20CC   | 65536     | 18.46     | **有缓存** | 4,140    | 82.3%    | 787.5             | 80.5%        |
| 64K   × 30CC | 65536     | 26.48     | 无缓存     | 40,191   | /        | 466.5             | /            |
| 64K  × 30CC  | 65536     | 27.11     | **有缓存** | 4,980    | 87.6%    | 1155.3            | 147.7%       |
| 128K × 10CC  | 131072    | 9.16      | 无缓存     | 35,017   | /        | 173.3             | /            |
| 128K × 10CC  | 131072    | 9.15      | **有缓存** | 5,775    | 83.5%    | 367.6             | 112.1%       |
| 128K  × 20CC | 131072    | 17.64     | 无缓存     | 83,090   | /        | 177.9             | /            |
| 128K  × 20CC | 131072    | 17.97     | **有缓存** | 12,235   | 85.3%    | 558.8             | 214.1%       |

作者：张贵楠、宋灿、寇希栋
