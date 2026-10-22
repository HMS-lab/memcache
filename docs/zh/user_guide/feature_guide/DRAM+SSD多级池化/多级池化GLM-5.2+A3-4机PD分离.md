# 多级池化GLM-5.2+A3-4机PD分离

## 概述

[MemCache](https://gitcode.com/Ascend/memcache) 是面向 AI 推理场景的分布式 KV Cache 缓存系统，针对昇腾超节点和服务器进行了性能优化，可作为 vLLM Ascend 等推理框架的 KV Pool 后端，为长文本和高前缀复用场景提供跨请求 KV Cache 存储与复用能力。

本文以四台 910C（Atlas 800I A3）组成的单实例 PD 分离部署为主线，模型为 GLM-5.2-W4A8，软件版本为 vLLM Ascend 0.23.0 RC1。200K 高性能服务和性能测试采用 2 个 Prefill 节点、2 个 Decode 节点；两组测试均启用 MMC，并对比“仅 MMC 内存池化”和“MMC + SSD 池化”两种方案。

本次测试的 MMC DRAM 缓存池为单机 300 GB、四机合计 1.2 TB；SSD 池化方案额外使用单机 3.5 TB、四机合计 14 TB 的 SSD 容量。

## 部署准备

### 前置要求

| 项目 | 要求或本文配置 |
| --- | --- |
| 硬件 | Atlas 800I A3（910C）× 4 |
| 部署拓扑 | 单实例 PD 分离：2 个 Prefill 节点 + 2 个 Decode 节点 |
| 驱动固件 | >= 25.5.1 |
| L1 1520 CC | >= 1.5.0 |
| OS 内核版本 | >= 5.10 |
| 模型 | GLM-5.2-W4A8 |
| 推理框架 | vLLM Ascend 0.23.0 RC1 |
| MMC DRAM 容量 | 单机预留 300 GB，四机物理合计 1,200 GB（约 1.2 TB） |
| SSD 容量 | 仅 SSD 池化方案需要；单机提供 3.5 TB 独占可用容量，四机物理合计 14 TB |

> SSD 池化使用的整盘、分区或 loop 块设备必须由 UBS IO 独占。配置前应确认设备没有挂载点和需要保留的数据；设备路径以各节点现场检查结果为准。

## 性能测试

本节给出 910C 四机 PD 分离单实例上，GLM-5.2-W4A8 在 200K 序列长度下的两组缓存方案对比。两组测试均启用 MMC，分别验证仅 MMC 内存池化与 MMC + SSD 池化；每组按 100、150、200、250、300 并发依次测试。

### 测试配置与容量口径

| 配置项 | 配置值 |
| --- | --- |
| 硬件 | 910C × 4 |
| 部署形态 | 四机 PD 分离、单实例 |
| 模型 | GLM-5.2-W4A8 |
| 软件版本 | vLLM Ascend 0.23.0 RC1 |
| 序列长度 | 200K |
| 并发数 | 100、150、200、250、300 |
| MMC | 两组方案均启用 |
| MMC 缓存池 | 单机 300 GB，四机合计 1,200 GB（约 1.2 TB） |
| SSD 池化缓存 | 单机 3.5 TB，四机合计 14 TB；仅 SSD 池化方案启用 |

### 总览：同并发结果对比

MMC + SSD 池化方案在五个并发档位上的成功率均高于仅 MMC 内存池化方案，差值随并发提升总体扩大；300 并发时，成功率由 84.83% 提升到 93.27%，失败请求数由 455 降至 202。平均响应时间和平均 TTFT 也均低于同并发下的仅 MMC 内存池化方案。

#### 成功率、平均响应时间和吞吐

| 并发数 | MMC 内存池化成功率（%） | MMC + SSD 池化成功率（%） | 成功率提升比（%） | MMC 内存池化平均响应时间（秒） | MMC + SSD 池化平均响应时间（秒） | 平均响应时间提升比（%） | MMC 内存池化输出吞吐（token/s） | MMC + SSD 池化输出吞吐（token/s） | 输出吞吐提升比（%） |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 90.60 | 93.00 | 2.65 | 69.342 | 49.216 | 29.02 | 177.07 | 230.93 | 30.42 |
| 150 | 90.27 | 94.20 | 4.35 | 78.109 | 52.416 | 32.89 | 279.23 | 356.16 | 27.55 |
| 200 | 89.60 | 95.05 | 6.08 | 85.145 | 55.844 | 34.41 | 340.49 | 445.92 | 30.96 |
| 250 | 88.92 | 94.44 | 6.21 | 85.299 | 69.103 | 18.99 | 356.96 | 544.44 | 52.52 |
| 300 | 84.83 | 93.27 | 9.95 | 97.068 | 79.558 | 18.04 | 424.11 | 651.92 | 53.71 |

#### TTFT

| 并发数 | MMC 内存池化 Avg TTFT（秒） | MMC + SSD 池化 Avg TTFT（秒） | Avg TTFT 提升比（%） | MMC 内存池化 P90 TTFT（秒） | MMC + SSD 池化 P90 TTFT（秒） | P90 TTFT 提升比（%） | MMC 内存池化 P95 TTFT（秒） | MMC + SSD 池化 P95 TTFT（秒） | P95 TTFT 提升比（%） |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 45.969 | 30.904 | 32.77 | 127.781 | 60.862 | 52.37 | 175.651 | 99.345 | 43.44 |
| 150 | 54.736 | 37.742 | 31.05 | 139.828 | 69.112 | 50.57 | 208.798 | 116.691 | 44.11 |
| 200 | 60.599 | 43.543 | 28.15 | 159.877 | 83.825 | 47.57 | 217.639 | 120.340 | 44.71 |
| 250 | 58.840 | 55.826 | 5.12 | 145.336 | 110.966 | 23.65 | 199.130 | 143.664 | 27.85 |
| 300 | 67.138 | 63.895 | 4.83 | 164.029 | 126.159 | 23.09 | 211.876 | 161.919 | 23.58 |

### 仅 MMC 内存池化测试结果

本组仅启用 MMC 内存池化，不启用 SSD 池化缓存。MMC 缓存池为单机 300 GB、四机合计 1.2 TB。

| 指标 | 100 并发 | 150 并发 | 200 并发 | 250 并发 | 300 并发 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 测试时间 | `20260812_224311` | `20260812_234333` | `20260813_004355` | `20260813_014417` | `20260813_024438` |
| 总请求数 | 1,000 | 1,500 | 2,000 | 2,500 | 3,000 |
| 成功数 | 906 | 1,354 | 1,792 | 2,223 | 2,545 |
| 失败数 | 94 | 146 | 208 | 277 | 455 |
| 成功率（%） | 90.60 | 90.27 | 89.60 | 88.92 | 84.83 |
| 总耗时（秒） | 3,009.51 | 3,009.72 | 3,010.19 | 3,010.22 | 3,009.47 |
| 并发数 | 100 | 150 | 200 | 250 | 300 |
| QPS（每秒请求数） | 0.33 | 0.50 | 0.66 | 0.83 | 1.00 |
| QPM（每分钟请求数） | 19.94 | 29.90 | 39.86 | 49.83 | 59.81 |
| 平均响应时间（秒） | 69.342 | 78.109 | 85.145 | 85.299 | 97.068 |
| 最大响应时间（秒） | 301.035 | 300.988 | 300.994 | 301.003 | 300.987 |
| 最小响应时间（秒） | 2.544 | 4.146 | 4.640 | 4.463 | 1.696 |
| 总输入 Tokens | 46,214,017 | 68,701,358 | 90,540,928 | 111,100,229 | 127,842,078 |
| 总输出 Tokens | 532,901 | 840,392 | 1,024,940 | 1,074,518 | 1,276,356 |
| 平均输入 Tokens | 46,214.0 | 45,800.9 | 45,270.5 | 44,440.1 | 42,614.0 |
| 平均输出 Tokens | 588.2 | 620.7 | 572.0 | 483.4 | 501.5 |
| 平均 TTFT（秒） | 45.969 | 54.736 | 60.599 | 58.840 | 67.138 |
| P50 TTFT（秒） | 20.845 | 29.429 | 33.166 | 35.488 | 43.510 |
| P90 TTFT（秒） | 127.781 | 139.828 | 159.877 | 145.336 | 164.029 |
| P95 TTFT（秒） | 175.651 | 208.798 | 217.639 | 199.130 | 211.876 |
| 响应时间 P50（秒） | 24.000 | 33.608 | 39.331 | 40.942 | 50.113 |
| 响应时间 P90（秒） | 272.511 | 281.337 | 300.061 | 300.438 | 300.644 |
| 响应时间 P95（秒） | 300.923 | 300.876 | 300.860 | 300.818 | 300.804 |
| 响应时间 P99（秒） | 300.996 | 300.972 | 300.975 | 300.965 | 300.945 |

### MMC + SSD 池化测试结果

本组启用 MMC 内存池化和 SSD 池化缓存。MMC 缓存池仍为单机 300 GB、四机合计 1.2 TB；SSD 池化缓存为单机 3.5 TB、四机合计 14 TB。

| 指标 | 100 并发 | 150 并发 | 200 并发 | 250 并发 | 300 并发 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 测试时间 | `20260813_113213` | `20260813_123234` | `20260813_133255` | `20260813_143317` | `20260813_153339` |
| 总请求数 | 1,000 | 1,500 | 2,000 | 2,500 | 3,000 |
| 成功数 | 930 | 1,413 | 1,901 | 2,361 | 2,798 |
| 失败数 | 70 | 87 | 99 | 139 | 202 |
| 成功率（%） | 93.00 | 94.20 | 95.05 | 94.44 | 93.27 |
| 总耗时（秒） | 3,009.87 | 3,009.56 | 3,009.54 | 3,010.22 | 3,010 |
| 并发数 | 100 | 150 | 200 | 250 | 300 |
| QPS（每秒请求数） | 0.33 | 0.50 | 0.66 | 0.83 | 1.00 |
| QPM（每分钟请求数） | 19.93 | 29.90 | 39.87 | 49.83 | 59.80 |
| 平均响应时间（秒） | 49.216 | 52.416 | 55.844 | 69.103 | 79.558 |
| 最大响应时间（秒） | 301.074 | 301.006 | 301.082 | 300.996 | 301.003 |
| 最小响应时间（秒） | 3.776 | 3.596 | 3.457 | 3.196 | 4.381 |
| 总输入 Tokens | 47,870,136 | 72,334,504 | 97,730,861 | 120,439,789 | 144,153,609 |
| 总输出 Tokens | 695,070 | 1,071,887 | 1,342,005 | 1,638,895 | 1,962,265 |
| 平均输入 Tokens | 47,870.1 | 48,223.0 | 48,865.4 | 48,175.9 | 48,051.2 |
| 平均输出 Tokens | 747.4 | 758.6 | 705.9 | 694.2 | 701.3 |
| 平均 TTFT（秒） | 30.904 | 37.742 | 43.543 | 55.826 | 63.895 |
| P50 TTFT（秒） | 20.568 | 27.172 | 31.994 | 41.817 | 49.054 |
| P90 TTFT（秒） | 60.862 | 69.112 | 83.825 | 110.966 | 126.159 |
| P95 TTFT（秒） | 99.345 | 116.691 | 120.340 | 143.664 | 161.919 |
| 响应时间 P50（秒） | 21.623 | 28.475 | 33.117 | 44.223 | 53.767 |
| 响应时间 P90（秒） | 122.113 | 123.144 | 117.018 | 145.991 | 182.307 |
| 响应时间 P95（秒） | 300.870 | 300.763 | 280.600 | 300.654 | 300.656 |
| 响应时间 P99（秒） | 300.984 | 300.963 | 300.920 | 300.935 | 300.928 |

### 环境准备

本文使用 GLM-5.2-W4A8；以下 ModelScope 仓库和示例部署目录仍沿用其制品名称 `GLM-5.2-w4a8c8`：
[*https://modelscope.cn/models/Eco-Tech/GLM-5.2-w4a8c8*](https://modelscope.cn/models/Eco-Tech/GLM-5.2-w4a8c8)

拉取 vLLM Ascend 0.23.0 RC1 镜像：
``docker pull quay.io/ascend/vllm-ascend:v0.23.0rc1``
以下保存为start-docker.sh脚本

```shell
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

## MemCache配置与启动

### 安装指定版本包

在修改 MemCache 配置前，先在四台节点的容器中安装指定版本包：

```bash
pip install memfabric_hybrid==1.2.0 --force-reinstall --no-deps
pip install memcache_hybrid==1.2.0 --force-reinstall --no-deps
```

`--force-reinstall` 会覆盖当前环境中的同名包，`--no-deps` 不会自动安装或调整依赖。安装后在四台容器中读取本次 pip 安装包内的 `VERSION`，确认完整 commit 与本次测试基线一致，并且四台节点之间没有差异：

```bash
python3 - <<'PY'
from importlib.util import find_spec
from pathlib import Path

for package in ("memfabric_hybrid", "memcache_hybrid"):
    spec = find_spec(package)
    version_file = Path(spec.origin).resolve().parent / "VERSION"
    print(f"[{package}]")
    print(version_file.read_text().strip())
PY
```

PyPI 发布的 `1.2.0` CPython 3.12 AArch64 wheel 参考值为：`memfabric_hybrid` commit 前 6 位 `999034`，`memcache_hybrid` commit 前 6 位 `4310d6`。

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

## 数据淘汰低水位：淘汰完成后的目标空间使用率
ock.mmc.evict_threshold_low = 50

## 数据淘汰高水位：空间使用率达到该值时，触发淘汰
ock.mmc.evict_threshold_high = 70

## DRAM压力水位：达到该值后，淘汰时快速删除数据释放内存
ock.mmc.rewarm.dram_watermark = 90

## Log level: debug, info, warn, error
ock.mmc.log_level = info
```

> [!TIP] 注意
> 以上3处 127.0.0.1 需改为启动meta_service服务节点IP。

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

> [!TIP] 注意
> 其中2处 127.0.0.1 需改为启动meta_service服务节点IP

### 所有节点清理缓存空间

**若部署服务前已reboot重启过节点，则可跳过此步骤。**

系统缓存空间中可能占用了部分memcache可用内存地址，执行free -h命令可查看系统缓存空间占用情况，需执行以下命令清理缓存，给memcache分配更多的可用空间。**注意**：若出现之前拉起模型服务速度快，清理缓存后拉起模型服务的速度变慢属于正常现象，因为缓存中的模型权重被清空，需重新加载，时间变长。

echo 3 > /proc/sys/vm/drop_caches

清理完成后，可执行free -h查看buff/cache值是否降低。

或通过脚本扫描可用空间，请参考以下链接
[memfabric-hybrid - Wiki - AtomGit](https://gitcode.com/Ascend/memfabric_hybrid/wiki/FAQ.md#2-%E8%B0%83%E7%94%A8halmemcreate%E8%BF%94%E5%9B%9E%E9%94%99%E8%AF%AF%E7%A0%81-6)

### 启动主进程

> [!TIP] 注意
> 本操作只需在MetaService启动节点上执行，启动一个MetaService服务即可，不需指定具体目录执行。

```bash
export MMC_META_CONFIG_PATH=/usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-meta.conf
#启动mmc-meta
nohup python3 -c "from memcache_hybrid import MetaService; MetaService.main()" >> /data/mmc-log/run_meta_service.log 2>&1 &
```

### 所有节点启动独立进程占用memcache可用空间

**注意：四台节点均执行以下步骤，每台节点拉起一个独立进程。**

**步骤一**：**复制一份mmc-local配置文件**​，用于memcache独立进程启动使用，并修改其申请的内存容量。

```bash
mkdir -p /data/deploy_glm5.2_dp32tp1
cp /usr/local/python3.12.13/lib/python3.12/site-packages/memcache_hybrid/config/mmc-local.conf /data/deploy_glm5.2_dp32tp1/mmc-local.conf
```

**步骤二**：对拷贝后的mmc-local.conf做出如下修改：

```bash
vim /data/deploy_glm5.2_dp32tp1/mmc-local.conf
```

修改配置：

```bash
ock.mmc.local_service.dram.size = 300GB
```

说明：每个节点根据实际情况进行设置，最终可用kv pool空间为4台A3节点设置的dram.size空间总和，如4台节点都设置为300GB，则最终可用池化空间为300GB \* 4 = 1200GB。每台节点占用空间可以不同，本次测试为300GB。

SSD 池化方案在每台节点使用两个 loop 设备，分别属于不同物理盘，每个 loop 设备 1.75 TB，单机合计 3.5 TB、四机物理容量合计 14 TB；仅 MMC 内存池化方案不启用 SSD 池化。

**步骤三：在每台宿主机准备双 loop 设备。** 以下示例使用两个不同的 SSD 后端目录，`1792G` 对应约 1.75 TB；必须先把两个目录占位符替换为本机实际路径，并确认各自有足够可用空间：

```bash
fallocate -l 1792G <SSD0_BACKING_DIR>/ubsio-loop0.img
fallocate -l 1792G <SSD1_BACKING_DIR>/ubsio-loop1.img

LOOP0=$(losetup --find --show --direct-io=on <SSD0_BACKING_DIR>/ubsio-loop0.img)
LOOP1=$(losetup --find --show --direct-io=on <SSD1_BACKING_DIR>/ubsio-loop1.img)
echo "$LOOP0"
echo "$LOOP1"
```

loop 编号由系统动态分配，不能预设为 `/dev/loop0`、`/dev/loop1`。写入配置前，在宿主机核对实际映射、容量、Direct I/O、挂载和签名状态：

```bash
losetup --list --output NAME,BACK-FILE,SIZE,DIO
lsblk -o NAME,TYPE,SIZE,FSTYPE,MOUNTPOINTS "$LOOP0" "$LOOP1"
findmnt -S "$LOOP0"
findmnt -S "$LOOP1"
```

预期两个loop设备均无挂载点、无需要保留的数据，并且 `BACK-FILE` 分别指向本机两个 1.75 TB 镜像文件；镜像文件应位于计划使用的 SSD物理盘。两块本地 SSD 的合计随机4MB IO读带宽建议不低于 7 GB/s。loop 设备需在宿主机创建；如果容器已提前启动，本文使用的特权 Docker 容器需要执行 `docker restart <容器名>` 后，再在容器内确认两个实际 loop 设备均可见。

**步骤四：仅修改独立缓存进程使用的 `/data/deploy_glm5.2_dp32tp1/mmc-local.conf`。** A3 分离模式每台节点只有该独立进程启用 SSD 池化，因此 `device_count` 固定为 `1`：

```ini
ock.mmc.local_service.storage.enabled = true
ubsio.disk.path = <本机实际LOOP0路径>:<本机实际LOOP1路径>
ubsio.mem.size_in_gb = 80
ubsio.standalone.device_count = 1
ubsio.standalone.force_new_disk = true
```

其中 `80GB` 采用A3 分离模式典型值；使用前需确认节点具有足够剩余内存。`force_new_disk = true` 会把两个专用 loop 设备按新盘初始化，不恢复旧 UBS IO 元数据，因此只能用于已确认可覆盖的专用镜像文件。

本次实测使用 `ubsio.mem.size_in_gb = 80`，Meta 水位使用 `50/70/90`。特性原理和完整参数说明参见 [DRAM + SSD 多级池化特性介绍](https://gitcode.com/Ascend/memcache/wiki/DRAM%20+%20SSD%20%E5%A4%9A%E7%BA%A7%E6%B1%A0%E5%8C%96%E7%89%B9%E6%80%A7%E4%BB%8B%E7%BB%8D.md) 与 [DRAM + SSD 多级池化配置指南](https://gitcode.com/Ascend/memcache/wiki/DRAM%20+%20SSD%20%E5%A4%9A%E7%BA%A7%E6%B1%A0%E5%8C%96%E9%85%8D%E7%BD%AE%E6%8C%87%E5%8D%97.md)。

**步骤五**：新建独立进程启动脚本。

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

**步骤六**：启动独立进程。

```bash
export MMC_LOCAL_CONFIG_PATH=/data/deploy_glm5.2_dp32tp1/mmc-local.conf
#启动独立进程
nohup python3 /data/deploy_glm5.2_dp32tp1/memcache.py >> /data/mmc-log/run_mmc-local.log 2>&1 &
```

**注意**：独立进程启动脚本执行完后，不要着急拉起GLM-5.2的vllm serve服务，查看/data/mmc-log/run_mmc-local.log日志，待4台节点均出现如下日志时，说明4台节点独立进程占用内存正常。若出现内存分配失败的报错，则需要调低ock.mmc.local_service.dram.size配置，重新拉起独立进程，直到4个独立进程均成功拉起。

```text
Successfully initialized memcache.
```

SSD 池化方案还需要检查本次启动日志：

```bash
grep -i "boostio" /data/mmc-log/run_mmc-local.log
```

能够检索到 `boostio` 相关日志，才表示该独立进程实际启动了 SSD 池化；没有匹配项或有报错时，应继续检查 `storage.enabled`、loop 设备可见性、`device_count`值是否正确 以及详细日志路径 `/var/log/ubsio/bio${PID}.log`。

也可以通过 `http://<P0_node_ip>:8000/metrics/summary` 页面快速访问确认总kv pool大小

## 部署 GLM-5.2 200K 高性能服务

本节对应文末性能测试采用的 200K 服务配置。脚本中的 `--max-model-len 210000` 为服务端最大模型长度配置，测试数据的序列长度为 200K。

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
        "kv_load_failure_policy": "recompute",
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
        "kv_load_failure_policy": "recompute",
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
