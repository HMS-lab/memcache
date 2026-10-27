# 环境变量说明

本文档列出 MemCache 使用的所有环境变量及其用途。

## 运行时环境变量

以下环境变量在程序运行时读取，用于控制运行时行为：

| 环境变量名 | 默认值 | 说明 |
|-----------|-------|------|
| `MMC_META_CONFIG_PATH` | 无（必填） | MetaService 配置文件路径，指向 `mmc-meta.conf`。未设置时 MetaService 无法启动。 |
| `MMC_LOCAL_CONFIG_PATH` | 无（必填） | LocalService 配置文件路径，指向 `mmc-local.conf`。LocalService 初始化时读取。 |
| `MMC_LOCAL_SERVICE_BACKEND_ID` | 无 | kv_event 后端标识（如 Pod IP）。配置项 `ock.mmc.local_service.backend_id` 为空时回退到此值。 |
| `MMC_ENABLE_PER_RANK_METRICS` | 未设置=关闭 | 开启 per-rank Prometheus 指标，取值 `1`/`true`/`ON`/`on`。仅 MetaService 进程生效。 |
| `MMC_DEVICE_ID` | 设备总数-1 | Python 示例使用的设备 ID，由 `run_all_examples.sh` 自动选择空闲设备并设置。 |

## HA / K8s 环境变量

以下环境变量在 K8S HA 部署模式下使用，用于 MetaService 多活选主与健康检查：

| 环境变量名 | 默认值 | 说明 |
|-----------|-------|------|
| `META_POD_NAME` | `meta-pod-0` | K8S Pod 名称，由 Downward API 注入，用于选主时标识当前 Pod。 |
| `META_NAMESPACE` | `default` | K8S 命名空间，选主时 Lease 资源所在命名空间。 |
| `META_LEASE_NAME` | `default` | K8S Lease 资源名称，多个 MetaService 实例通过竞争同一 Lease 实现选主。 |
| `META_POD_IP` | 无 | K8S Pod IP，由 Downward API 注入，用于 Liveness/Readiness 健康检查探针。 |

## 安装与库路径环境变量

以下环境变量由安装脚本 `set_env.sh` 设置，或由用户在运行前手动配置：

| 环境变量名 | 说明 |
|-----------|------|
| `MEMCACHE_HYBRID_HOME_PATH` | MemCache 安装根路径，由 `set_env.sh` 设置，典型值为 `/usr/local/memcache_hybrid/latest`。 |
| `LD_LIBRARY_PATH` | 动态库搜索路径，需包含 MemCache、MemFabric 和 CANN 的 `lib64` 目录。 |
| `ASCEND_HOME_PATH` | CANN 安装路径，MemFabric 底座加载 CANN 动态库时使用。典型值为 `/usr/local/Ascend/ascend-toolkit/latest`。 |

## 构建时环境变量

以下环境变量在编译构建时读取，用于控制构建行为：

| 环境变量名 | 默认值 | 说明 |
|-----------|-------|------|
| `MMC_BUILD_JOBS` | `32` | 并行编译线程数，影响 `make -j` / `ninja -j` 的并行度。 |
| `PYTHON_HOME` | `/usr/local/python3.11` 或 `/usr/local` | Python 安装路径，用于 pybind11 编译和 wheel 打包。未设置时自动探测。 |
| `MEMCACHE_VERSION` | 从 `VERSION` 文件读取 | 包版本号，用于生成 whl 包版本。**仅构建 Python whl 包时需要，C++ 构建不需要。** |
| `ENABLE_FUZZ` | `OFF` | 是否启用 Fuzz 构建。`run_fuzz.sh` 会设为 `ON`，CMake 通过 `$ENV{ENABLE_FUZZ}` 读取。 |
| `ASAN_OPTIONS` | 无 | AddressSanitizer 运行选项，UT 和 Fuzz 脚本设置默认值用于控制内存检查行为。 |

## 使用示例

### NPU 环境运行前设置

```bash
# 加载 CANN、MemFabric 和 MemCache 环境
source /usr/local/Ascend/ascend-toolkit/set_env.sh
source /usr/local/memfabric_hybrid/set_env.sh
source /usr/local/memcache_hybrid/set_env.sh

# 设置配置文件路径
export MMC_META_CONFIG_PATH=/usr/local/memcache_hybrid/latest/config/mmc-meta.conf
export MMC_LOCAL_CONFIG_PATH=/usr/local/memcache_hybrid/latest/config/mmc-local.conf
```

### whl 安装后运行前设置

```bash
# whl 安装后配置文件位于 site-packages 下
export MMC_META_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-meta.conf
export MMC_LOCAL_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-local.conf
```

### K8S HA 部署

```bash
# 用户侧设置配置文件路径
export MMC_META_CONFIG_PATH=/home/meta/config/mmc-meta.conf

# 以下变量由 K8S Downward API 自动注入，无需手动设置：
#   META_POD_NAME   — metadata.name
#   META_POD_IP     — status.podIP
#   META_NAMESPACE  — 命名空间
#   META_LEASE_NAME — Lease 资源名
```

### UT 构建运行

```bash
# ASCEND_HOME_PATH、LD_LIBRARY_PATH 等由 run_ut.sh 自动设置为 mock 路径
bash script/run_ut.sh

# 按名称过滤运行
bash script/run_ut.sh meta_service
```
