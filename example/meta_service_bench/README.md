# meta_service 性能基准测试

## 简介

本 benchmark 用于测试 **meta_service 元数据处理性能**。它将 meta_service 和模拟 local_service 拆分为多个子进程，支持多进程并发访问 metaService，测试压力下的元数据处理性能。

不使用真实的 local_service。每个客户端子进程模拟 local_service 行为：直接创建 `MetaNetClient`，注册 mock BM 资源（伪造 GVA 地址和容量），向 meta_service 发送 RPC 请求。所有 BM 数据读写操作均已打桩。

### 多进程架构

```text
┌─────────────────────────────────────────────────────────────┐
│                  Coordinator (main)                          │
│   1. fork meta_service 子进程                                 │
│   2. 等待 meta_service 就绪                                    │
│   3. fork N 个 client 子进程                                   │
│   4. 等待所有 client 完成                                       │
│   5. 从共享内存收集结果                                          │
│   6. 终止 meta_service，打印聚合报告                            │
├──────────────┬──────────────┬──────────────┐                 │
│ meta_service │  client[0]   │  client[N-1] │                 │
│ (子进程)      │  (子进程)     │  (子进程)     │                 │
│              │  MetaNetClient│  MetaNetClient│                 │
│ MmcMetaService│  + mock BM  │  + mock BM   │                 │
│  (进程内)     │  + M 线程    │  + M 线程     │                 │
└──────────────┴──────────────┴──────────────┘                 │
└─────────────────────────────────────────────────────────────┘
```

**关键设计：**

- meta_service 运行在独立子进程中，通过 `fork()` 创建
- 每个客户端子进程独立创建 `MetaNetClient`，注册独立 rank 的 mock BM 资源
- 通过 `mmap(MAP_SHARED | MAP_ANONYMOUS)` 共享内存收集各进程结果
- 不初始化 BM 库，不需要 NPU 硬件
- 所有 BM 数据拷贝操作被跳过，仅测量 meta_service 端的元数据处理

### 测试的操作类型

| 单个操作 | 批量操作 |
|---------|---------|
| AllocRequest → AllocResponse | BatchAllocRequest → BatchAllocResponse |
| UpdateRequest → Response | BatchGetRequest → BatchAllocResponse |
| GetRequest → AllocResponse | BatchUpdateRequest → BatchUpdateResponse |
| QueryRequest → QueryResponse | BatchQueryRequest → BatchQueryResponse |
| IsExistRequest → IsExistResponse | BatchIsExistRequest → BatchIsExistResponse |
| RemoveRequest → Response | BatchRemoveRequest → BatchRemoveResponse |

## 前置条件

### 1. 编译安装 memcache

在 memcache 源码根目录执行：

```bash
bash script/build_and_pack_run.sh --build_mode DEBUG --incremental
```

编译产物在 `output/` 目录下。

### 2. 安装 memfabric_hybrid run 包

```bash
bash memfabric_hybrid-*.run          # 安装 memfabric_hybrid
source /usr/local/memfabric_hybrid/set_env.sh
```

## 编译

### 方式一：源码树内编译（推荐）

在 memcache 源码根目录，通过 `BUILD_CPP_EXAMPLE=ON` 启用 benchmark 编译：

```bash
# 1. CMake 配置
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=DEBUG \
    -DBUILD_CPP_EXAMPLE=ON \
    -S . -B build/

# 2. 编译 benchmark 目标
ninja -C build/ bench_meta_service
```

编译产物位于 `build/example/meta_service_bench/bench_meta_service`。

### 方式二：独立编译

在安装了 memcache 和 memfabric_hybrid 的 run 包后，可在本目录独立编译：

```bash
# 设置环境变量
export MEMCACHE_HYBRID_HOME_PATH=/path/to/memcache    # 源码根目录
source /usr/local/memfabric_hybrid/set_env.sh

cd example/meta_service_bench
mkdir build && cd build
cmake ..
make -j$(nproc)
```

编译产物为 `example/meta_service_bench/build/bench_meta_service`。

> **注意：** 独立编译模式链接动态库 `libmf_memcache.so`，需要该库导出内部 C++ 符号。源码树内编译模式链接静态库 `mmc_static`，符号完整可用，推荐使用。

## 运行

```bash
# 设置动态库搜索路径（源码树内编译时）
export LD_LIBRARY_PATH=$(pwd)/build/3rdparty/spdlog/lib:$(pwd)/build/3rdparty/spdlog/lib64:$LD_LIBRARY_PATH

# 运行
./build/example/meta_service_bench/bench_meta_service
```

### 命令行参数

```text
Usage: bench_meta_service [options]
Options:
  -p <processes>    Client process count          (default: 4)
  -t <threads>      Threads per process           (default: 4)
  -n <ops>          Operations per thread          (default: 5000)
  -k <keyspace>     Key space size                (default: 20000)
  -w <warmup>       Warmup key count              (default: 5000)
  -b <batchsize>    Batch size for batch ops      (default: 32)
  -s <blobsize>     Blob size in bytes            (default: 32768)
  --perf             Enable perf profiling of meta_service   (default: off)
  --perf-freq <hz>   perf record sample frequency             (default: 99)
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-p` | 客户端进程数（每个进程独立 rank） | 4 |
| `-t` | 每个进程的工作线程数 | 4 |
| `-n` | 每个线程执行的操作数 | 5000 |
| `-k` | Key 空间大小（所有进程共享同一 key 空间） | 20000 |
| `-w` | 预热阶段总 key 数量 | 5000 |
| `-b` | 批量操作每批的 key 数量 | 32 |
| `-s` | 单个 blob 大小（字节） | 32768 (32KB) |
| `--perf` | 对 meta_service 子进程启用 perf 采样（attach 模式），输出固定为 `perf.data` | 关闭 |
| `--perf-freq` | perf 采样频率（Hz） | 99 |

### 运行示例

```bash
# 2 进程 x 2 线程快速验证
./build/example/meta_service_bench/bench_meta_service -p 2 -t 2 -n 500 -k 2000 -w 500

# 8 进程 x 4 线程大规模压测（32 并发）
./build/example/meta_service_bench/bench_meta_service -p 8 -t 4 -n 10000 -k 50000 -w 20000

# 对 meta_service 采集 perf 数据，压测结束后用 perf report 分析
./build/example/meta_service_bench/bench_meta_service -p 4 -t 4 -n 5000 --perf --perf-freq 999
perf report -i perf.data

# 查看帮助
./build/example/meta_service_bench/bench_meta_service -h
```

## 输出说明

benchmark 执行分四个阶段：

1. **启动 meta_service**：进程内启动 `MmcMetaService`，监听 TCP 端口
2. **创建模拟 local_service**：创建 `MetaNetClient`，连接到 meta_service
3. **注册 mock BM**：发送 `BmRegisterRequest`，注册伪造的 HBM 资源（1GB）
4. **预热 + 压测**：先 Alloc+Update 预热 key，再多线程跑混合读写和批量操作

### 输出示例

```text
============================================================
  MetaService Multi-Process Benchmark
============================================================
Configuration:
  Processes:    2
  Threads/proc: 2
  Ops/thread:   200
  Total ops:    800
  Key space:    1000
  Blob size:    32768 bytes
  Warmup keys:  200
  Mock HBM/proc:1024 MB
  Workload:     Alloc=10% Update=10% Get=30% Query=20% IsExist=20% Remove=10%

[1/3] Starting meta_service process...
meta_service started (pid=47655)

[2/3] Forking 2 client processes...

[3/3] Waiting for 2 clients to finish...

========== Multi-Process Aggregated Report ==========

Processes: 2, Threads/proc: 2, Ops/thread: 200, Total ops: 800

+------------------------------------------------------------------------------------------------------------------------+
| Op         |  Success |     Fail |    Avg(us) |    P50(us) |    P95(us) |    P99(us) |    Min(us) |    Max(us) |
+------------------------------------------------------------------------------------------------------------------------+
| Alloc      |       56 |        0 |     175.46 |     170.00 |     204.00 |     257.00 |     143.00 |     302.00 |
| Update     |       88 |        0 |     153.48 |     120.00 |     162.50 |    1430.00 |     101.00 |    2671.00 |
| Get        |      260 |        0 |     147.02 |     125.00 |     169.50 |     230.50 |     100.00 |    2600.00 |
| Query      |      148 |        0 |     234.93 |     115.00 |     145.00 |    7662.00 |      98.00 |    7662.00 |
| IsExist    |      170 |        0 |     113.66 |     111.00 |     134.00 |     178.50 |      94.00 |     185.00 |
| Remove     |       78 |        0 |     320.47 |     122.50 |     185.50 |    7713.50 |      99.00 |    7714.00 |
+------------------------------------------------------------------------------------------------------------------------+

Total ops: 800 (success: 800, failure: 0)
Max duration (wall): 0.04 sec
Throughput:         22317.69 ops/sec

Per-process summary:
  Rank   |      PID |      Ops |     Succ |     Fail |   Duration
  0      |    47733 |      400 |      400 |        0 |       0.04
  1      |    47734 |      400 |      400 |        0 |       0.04

Benchmark finished.
```

### 指标说明

| 指标 | 说明 |
|------|------|
| Success / Fail | RPC 调用成功/失败次数（返回 `MMC_OK` 为成功） |
| Avg(us) | 平均延迟（微秒） |
| P50(us) | 中位数延迟（50 分位） |
| P95(us) | 95 分位延迟 |
| P99(us) | 99 分位延迟 |
| Min/Max(us) | 最小/最大延迟 |
| Throughput | 总吞吐量（ops/sec） |

### 工作负载分布

**混合读写（单操作）：**

| 操作 | 占比 | 说明 |
|------|------|------|
| Alloc | 10% | 向 meta_service 申请内存（分配器管理 + LRU 插入） |
| Update | 10% | 更新 blob 状态为已写入 |
| Get | 30% | 获取 blob 元数据（触发租约续期） |
| Query | 20% | 查询 key 的元数据信息 |
| IsExist | 20% | 检查 key 是否存在 |
| Remove | 10% | 删除元数据并释放内存（分配器回收 + LRU 擦除） |

**批量操作：** 每轮依次执行 BatchAlloc / BatchGet / BatchUpdate / BatchQuery / BatchIsExist / BatchRemove，每批 `-b` 个 key。
