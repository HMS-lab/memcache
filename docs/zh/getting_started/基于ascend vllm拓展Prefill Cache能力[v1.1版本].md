# 基于ascend vllm拓展Prefill Cache能力[v1.1版本]

## [Home](https://gitcode.com/Ascend/memcache/wiki/Home.md)

## 1.方案介绍

### 1.1背景

在大模型驱动的智能对话和内容生成应用中，如客服机器人或长文档摘要系统，用户往往需要处理大量文本或与模型进行多轮交互。这类场景有着大量重复内容且对响应速度有较高要求。为提升用户体验且控制部署成本，业界常用Prefix-cache技术，将历史数据缓存到HBM显存，以查代算提升性能。当前存在HBM空间有限的挑战，使用大显存推理卡的成本较高。通过HBM+DRAM的分级缓存可以大幅增加KVCache的缓存空间，提升命中率，有效缓解计算资源压力，显著降低TTFT（首token响应时延）。

本文将基于MemCache技术方案，介绍分级缓存的参考实践，帮助读者提高大模型推理服务性能。

## 2.部署方案配置

### 2.1软件版本

### 2.2典型配置

## 3.部署指导

### 3.1前置条件

已完成OS、昇腾HDK、Docker安装，并已下载对应大模型的权重文件和测试数据集，集群（多机互联）场景下需完成参数面组网，确保网络互通。

因为有镜像拉取和安装依赖，推荐服务器处于联网环境。

1. 本实践使用Qwen3-32B的int8量化为例进行测试。模型权重可从Hugging Face等社区下载后按照昇腾msmodelslim工具要求，进行W8A8量化。

    Hugging Face权重下载链接：<https://huggingface.co/Qwen/Qwen3-32B，https://huggingface.co/deepseek-ai/DeepSeek-R1-Distill-Qwen-32B>

2. HDK安装参考链接：<https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/850/softwareinst/instg/instg_0005.html?Mode=PmIns&InstallType=local&OS=openEuler>

3. 组网参考链接：<https://support.huawei.com/enterprise/zh/doc/EDOC1100543351/ebeddbba?idPath=23710424|251366513|254884019|261408772|258915651>

### 3.2 部署流程汇总

安装步骤：

|安装部署  |步骤说明  |
|--|--|
| 步骤1：创建vLLM Ascend容器 |拉取vLLM Ascend镜像，创建容器  |
| 步骤2：安装MemCache |容器内安装MemCache  |
| 步骤3：拉起MetaService |配置mmc-meta.conf+mmc-local.conf文件，拉起MetaService |
| 步骤4：拉起开启MemCache特性的vLLM Ascend服务 |拉起命令中，配置相关环境变量并配置开启MemCache |

### 3.3  详细部署操作

#### 3.3.1 创建vLLM Ascend容器

本文介绍使用容器部署vLLM Ascend的方式，镜像下载地址：<https://quay.io/repository/ascend/vllm-ascend?tab=tags>

若需采用其它安装方式（使用Python安装），请参考官方安装指导：<https://docs.vllm.ai/projects/ascend/zh-cn/v0.13.0/installation.html#set-up-using-docker>

单机部署和双机部署，均参考如下操作步骤：

```bash
# docker拉取镜像
# 也可以采用下载镜像的tar包，然后使用docker load -i xx.tar的方式安装镜像
docker pull quay.io/ascend/vllm-ascend:releases-v0.13.0-openeuler

# 根据实际下载镜像版本进行替换
export IMAGE=quay.io/ascend/vllm-ascend:releases-v0.13.0-openeuler

# 查看NPU设备号
ls /dev/davinci*
# 根据查询到的davinci设备号，配置容器创建时的device参数
/dev/davinci0  /dev/davinci1  /dev/davinci2  /dev/davinci3  /dev/davinci4  /dev/davinci5  /dev/davinci6  /dev/davinci7  /dev/davinci_manager

# 创建容器，注意权重挂载路径根据真实情况替换，NPU卡数据根据实际情况替换（此处以8卡为例）
docker run -dit \
    --name vllm-ascend-memcache \# 依据实际情况配置
    --shm-size=500g \
    --net=host \
    --device /dev/davinci0 \# 依据实际情况配置，执行时删除注释，不要有空格
    --device /dev/davinci1 \
    --device /dev/davinci2 \
    --device /dev/davinci3 \
    --device /dev/davinci4 \
    --device /dev/davinci5 \
    --device /dev/davinci6 \
    --device /dev/davinci7 \
    --device /dev/davinci_manager \
    --device /dev/devmm_svm \
    --device /dev/hisi_hdc \
    -v /usr/local/dcmi:/usr/local/dcmi \
    -v /usr/local/Ascend/driver/tools/hccn_tool:/usr/local/Ascend/driver/tools/hccn_tool \
    -v /usr/local/bin/npu-smi:/usr/local/bin/npu-smi \
    -v /usr/local/Ascend/driver/lib64/:/usr/local/Ascend/driver/lib64/ \
    -v /usr/local/Ascend/driver/version.info:/usr/local/Ascend/driver/version.info \
    -v /etc/ascend_install.info:/etc/ascend_install.info \
    -v /dev/hugepages:/dev/hugepages \# 挂载大页内存
    -v /data/weights:/root/.cache \# 根据真实情况替换，执行时删除注释，不要有空格
    -it $IMAGE bash

# 进入容器
docker exec -ti vllm-ascend-memcache bash
```

#### 3.3.2 MemCache安装

`pip install memcache_hybrid`
其他安装方式可参考[安装](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/install_whl.md)

#### 3.3.3 配置配置文件+拉起MetaService服务

MemCache特性有2个配置文件（mmc-meta.conf和mmc-local.conf）需要配置。其中mmc-meta.conf需要在MetaService拉起前完成配置，mmc-local.conf需要在拉起vLLM Ascend前完成配置。

双机场景，双机使用相同的配置文件内容，并只需要在主机拉起MetaService。

1. 进入容器：

    ```bash
    # 进入容器,容器名根据实际情况配置
    docker exec -ti vllm-ascend-memcache bash
    ```

2. 创建配置文件：

    mmc-meta.conf配置文件

    ```ini
    # 双机场景，在主节点和从节点上，以下IP地址都需配置为主节点的IP地址
    # 端口号可按需修改，需要同步修改mmc-local.conf，保持一致
    ock.mmc.meta_service_url = tcp://xx.xx.xx.xx:5000
    ock.mmc.meta_service.config_store_url = tcp://xx.xx.xx.xx:6000
    ock.mmc.meta_service.metrics_url = http://xx.xx.xx.xx:8000
    ock.mmc.log_level = info
    ```

    mmc-local.conf配置文件，按需配置NPU卡数和MemCache占用的DRAM内存大小。

    ```ini
    # 以下IP地址，端口号都需和mmc-meta.conf保持一致
    ock.mmc.meta_service_url = tcp://xx.xx.xx.xx:5000
    ock.mmc.local_service.config_store_url = tcp://xx.xx.xx.xx:6000
    ock.mmc.log_level = info

    # 配置总NPU卡数（多机场景配所有设备的总NPU卡数）
    ock.mmc.local_service.world_size = 256

    # 300I A2场景（单机），配置为host_shm
    # 800I A2场景（单双机），配置为device_rdma
    ock.mmc.local_service.protocol = device_rdma

    # 表示给每卡分配的内存空间，例如配置128G，则使用2卡的服务会占用共256G系统内存
    # 注意预留足够的空间给系统应用，剩余内存空间可分配给MemCache使用
    ock.mmc.local_service.dram.size = 128GB
    ```

    更多配置参数可参考[配置](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/memcache_config.md)

3. 拉起metaservice服务（双机部署时，只有主机需要拉起）。

    ```python
    # 使用python脚本方式，拉起metaservice服务
    python3 metaservice.py
    ```

    脚本内容如下（metaservice.py）

    ```python
    from memcache_hybrid import MetaService
    MetaService.main()
    ```

    更多拉起方式可参考[拉起metaservice](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/install_run.md)

#### 3.3.4  拉起开启MemCache特性的vLLM Ascend服务

##### 场景1：单机服务

拉起单机vLLM Ascend服务，增加开启memcache特性的参数，原有业务的配置参数（如卡号，权重等）保持不变。

```ini
source /usr/local/Ascend/ascend-toolkit/set_env.sh
source /usr/local/Ascend/nnal/atb/set_env.sh

export MMC_LOCAL_CONFIG_PATH=/usr/local/memcache_hybrid/latest/config/mmc-local.conf
export VLLM_USE_V1=1
export ASCEND_RT_VISIBLE_DEVICES=2,3 # 使用的NPU卡号
export ACL_OP_INIT_MODE=1
export PYTORCH_NPU_ALLOC_CONF="expandable_segments:True"
export PYTHONHASHSEED=0
export HCCL_BUFFSIZE=1024

python -m vllm.entrypoints.openai.api_server \
  --model=/root/.cache/DeepSeek-R1-Distill-Qwen-32B-w8a8 \# 修改实际的权重路径
  --served-model-name qwen3 \
  --trust-remote-code \
  --async-scheduling \
  --quantization ascend \
  --distributed-executor-backend mp \
  --tensor-parallel-size 2 \
  --port 8113 \
  --max-model-len 32K \
  --compilation_config '{"cudagraph_mode":"FULL_DECODE_ONLY"}' \
  --additional_config='{"ascend_scheduler_config":{"enabled":false}, "enable_shared_expert_dp":false, "chunked_prefill_for_mla":true}' \
  --gpu-memory-utilization 0.90 \
  --kv-transfer-config \# 使能MemCache特性
  '{
      "kv_connector": "AscendStoreConnector",
      "kv_role": "kv_both",
      "kv_connector_extra_config": {
        "backend": "memcache",
        "mooncake_rpc_port":"0"
      }
  }' > log_vllm_memcache.log 2>&1
```

开启MemCache特性的vLLM Ascend服务，在服务的运行日志中会新增External prefix cache hit rate关键字，用于指示MemCache命中率（优先显存命中，显存未命中而内存命中时，统计此命中率）。

单机多个服务开启MemCache场景，保持mmc-meta.conf和mmc-local.conf文件内容相同，可以只拉起1个MetaService，然后拉起多个推理服务。

##### 场景2：双机服务

双机DeepSeek-V3服务开启MemCache场景，只需要在主机拉起MetaService，然后在原有双机拉起命令上，增加开启MemCache特性的参数。

双机拉起命令可参考官方文档：<https://docs.vllm.ai/projects/ascend/zh-cn/v0.13.0/tutorials/DeepSeek-V3.1.html>

权重可参考前置准备，使用modelslim进行转换。

双机拉起DeepSeek的参考命令如下，假设主节点的IP为xx.xx.xx.0，对应网卡名为xxx0；从节点IP为xx.xx.xx.1，对应网卡名为xxx1。

**主节点** 拉起命令:

```ini
source /usr/local/Ascend/ascend-toolkit/set_env.sh
source /usr/local/Ascend/nnal/atb/set_env.sh

export MMC_LOCAL_CONFIG_PATH=/usr/local/memcache_hybrid/latest/config/mmc-local.conf

# 可通过ifconfig获得以下参数
# nic_name为local_ip对应的网卡名，例如enp***，local_ip为当前节点的通信IP
nic_name="xxx0"
local_ip="xx.xx.xx.0"

# node0_ip配置为主节点IP，
node0_ip="xx.xx.xx.0"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=10
export PYTHONHASHSEED=0
export HCCL_BUFFSIZE=1024
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export VLLM_USE_V1=1

vllm serve /root/.cache/DeepSeek-V3.1-w8a8-mtp-QuaRot \
  --host 0.0.0.0 \
  --port 8004 \
  --enforce-eager \
  --data-parallel-size 2 \
  --data-parallel-size-local 1 \
  --api-server-count 2 \
  --data-parallel-address $node0_ip \
  --data-parallel-rpc-port 13389  \
  --tensor-parallel-size 8 \
  --seed 1024 \
  --served-model-name deepseek \
  --max-model-len 65536 \
  --max-num-batched-tokens 16384 \
  --trust-remote-code \
  --gpu-memory-utilization 0.9 \
  --quantization ascend \
  --max-num_seqs 20 \
  --enable-expert-parallel \
  --no-enable-prefix-caching \
  --additional_config='{"ascend_scheduler_config":{"enabled":false}, "enable_shared_expert_dp":false, "chunked_prefill_for_mla":true}' \
  --kv-transfer-config \
  '{
        "kv_connector": "AscendStoreConnector",
        "kv_role": "kv_both",
        "kv_connector_extra_config": {
                "backend": "memcache",
                "mooncake_rpc_port":"0"
           }
  }' > log_hunbu_0.log 2>&1
```

**从节点** 拉起命令：

```ini
source /usr/local/Ascend/ascend-toolkit/set_env.sh
source /usr/local/Ascend/nnal/atb/set_env.sh

export MMC_LOCAL_CONFIG_PATH=/usr/local/memcache_hybrid/latest/config/mmc-local.conf

# 可通过ifconfig获得以下参数
# nic_name为local_ip对应的网卡名，例如enp***，local_ip为当前节点的通信IP
nic_name="xxx1"
local_ip="xx.xx.xx.1"

# node0_ip配置为主节点IP，
node0_ip="xx.xx.xx.0"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=10
export PYTHONHASHSEED=0
export HCCL_BUFFSIZE=1024
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export VLLM_USE_V1=1

vllm serve /weights/DeepSeek-V3.1-w8a8-mtp-QuaRot \
  --host 0.0.0.0 \
  --port 8004 \
  --headless  \
  --enforce-eager \
  --data-parallel-size 2 \
  --data-parallel-size-local 1 \
  --data-parallel-start-rank 1 \
  --data-parallel-address $node0_ip \
  --data-parallel-rpc-port 13389  \
  --tensor-parallel-size 8 \
  --seed 1024 \
  --served-model-name deepseek \
  --max-model-len 65536 \
  --max-num-batched-tokens 16384 \
  --trust-remote-code \
  --gpu-memory-utilization 0.9 \
  --quantization ascend \
  --max-num_seqs 20 \
  --enable-expert-parallel \
  --no-enable-prefix-caching \
  --additional_config='{"ascend_scheduler_config":{"enabled":false}, "enable_shared_expert_dp":false, "chunked_prefill_for_mla":true}' \
  --kv-transfer-config \
   '{
        "kv_connector": "AscendStoreConnector",
        "kv_role": "kv_both",
        "kv_connector_extra_config": {
                "backend": "memcache",
                "mooncake_rpc_port":"0"
           }
  }' > log_hunbu_1.log 2>&1
```
