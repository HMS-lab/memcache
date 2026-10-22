# MemCache Client 性能指标看板

## 1. 简介

MemCache 提供了开箱即用的 **Client 端指标看板**，覆盖各 Rank 的 BATCH_PUT / BATCH_GET 操作延时、数据传输带宽及 UBS IO  存储资源等核心运维指标。看板的指标数据来源于 Client 端内置的指标采集框架，通过 30s 周期 Push 上报至 Meta Service，经 `/metrics` 端点以 Prometheus 格式对外暴露。

> **前置依赖**：Prometheus + Grafana 的部署流程和基础对接方式，请先参考 [memcache 对接 Grafana + Prometheus 使用教程](https://gitcode.com/Ascend/memcache/wiki/memcache%E5%AF%B9%E6%8E%A5Grafana+Prometheus%E4%BD%BF%E7%94%A8%E6%95%99%E7%A8%8B.md)。

## 2. 看板导入

### 2.1 配置 Prometheus

在 `prometheus.yml` 中添加指向 `/metrics` 端点的 scrape 配置：

```yaml
scrape_configs:
  - job_name: 'memcache-client'
    scrape_interval: 15s
    static_configs:
      - targets: ['<meta_host>:<port>']
    metrics_path: '/metrics'
```

### 2.2 导入 Grafana

1. 添加上述 Prometheus 为数据源
2. 导入预置 Dashboard：[example/metrics/memcache_dashboard_client_metrics.json](https://gitcode.com/Ascend/memcache/blob/master/example/metrics/memcache_dashboard_client_metrics.json)

## 3. 看板概览

### 3.1 PUT / GET Performance（30s Window）

用于观察各 Rank Client 端 30 秒窗口内的 PUT/GET 操作延时和瞬时带宽。

| 面板 | 对应操作 | 说明 |
|------|----------|------|
| **Get Latency** | BatchGet 操作 | Client 端 30 秒窗口内 BatchGet 操作的延迟分位数（P50 / P90 / P99 / Avg），单位秒 |
| **Get Bytes** | BatchGet 操作 | Client 端 30 秒窗口内 BatchGet 操作的传输字节数（totalBytes） |
| **Get Bandwidth** | BatchGet 操作 | Client 端 30 秒窗口内 BatchGet 操作的瞬时带宽（B/s） |
| **Put Latency** | BatchPut 操作 | Client 端 30 秒窗口内 BatchPut 操作的延迟分位数（P50 / P90 / P99 / Avg），单位秒 |
| **Put Bytes** | BatchPut 操作 | Client 端 30 秒窗口内 BatchPut 操作的传输字节数（totalBytes） |
| **Put Bandwidth** | BatchPut 操作 | Client 端 30 秒窗口内 BatchPut 操作的瞬时带宽（B/s） |

每个延迟面板按 `rank` × `quantile` 维度展示多条曲线：**P50 / P90 / P99 / Avg**，带宽面板按 `rank` 维度展示。可通过 Rank 筛选聚焦特定节点，通过 Quantile 筛选不同分布曲线。

### 3.2 PUT / GET Performance（Cumulative）

用于观察各 Rank Client 端自进程启动以来的累计传输量和生命周期平均吞吐。

| 面板 | 内容 | 说明 |
|------|------|------|
| **Cumulative Latency** | 累计 IO 耗时 | 进程启动以来总 IO 耗时（秒） |
| **Cumulative Bytes** | 累计传输字节数 | 进程启动以来总传输字节数 |
| **Cumulative Bandwidth** | 生命周期平均带宽 | 生命周期平均吞吐率（B/s） |

累计面板按 `rank` × `operation` 维度展示（batch_put / batch_get），支持 `$rank` 过滤。

### 3.3 UBS IO Storage

用于观察各 Rank Client 端 UBS IO WriteCache 的存储资源使用率、盘健康状态及每盘带宽。

| 面板 | 指标内容 | 说明 |
|------|----------|------|
| **SSD Capacity & Usage** | SSD 总容量 vs 已使用量 | 展示 `disk_capacity_bytes` 和 `disk_used_bytes`，观察 SSD 空间水位 |
| **Memory Capacity & Usage** | 内存总容量 vs 已使用量 | 展示 `mem_capacity_bytes` 和 `mem_used_bytes`，观察内存空间水位 |
| **Per-Disk Read Bandwidth** | 每盘读带宽 (B/s) | 以 `disk_path` 为标签逐盘展示读吞吐，快速定位读慢盘 |
| **Per-Disk Write Bandwidth** | 每盘写带宽 (B/s) | 以 `disk_path` 为标签逐盘展示写吞吐，快速定位写慢盘 |
| **Per-Disk Total Bandwidth** | 每盘总带宽 (B/s) | 以 `disk_path` 为标签逐盘展示总吞吐（读写合计） |
| **Disk Count & Faults** | 总盘数 vs 故障盘数 | 监控盘健康，故障盘数 > 0 触发告警 |

所有面板均支持 `$rank` 筛选，每盘带宽面板额外支持 `$disk_path` 筛选。

### 3.4 效果图展示

![image.png](https://raw.gitcode.com/user-images/assets/10261984/fa5e1ff3-0e1e-496f-82c4-f79b502e199d/image.png 'image.png')

![image.png](https://raw.gitcode.com/user-images/assets/7672915/1e883311-f43e-4c2d-81b6-92b6afbe443b/image.png 'image.png')

## 4. 看板使用方法

### 4.1 变量筛选

看板顶部提供四个下拉筛选器：

| 变量 | 说明 | 用法 |
|------|------|------|
| **Datasource** | 选择 Prometheus 数据源 | 导入时自动匹配，无需手动调整 |
| **Rank (Client)** | 按 Client Rank 筛选，支持多选/全选 | 影响下方所有面板（窗口带宽/延迟 + 累计 + UBS IO）。逐 Rank 勾选可定位单节点异常 |
| **Quantile** | 按延时分位数筛选，支持多选/全选 | 仅影响 30s Window 延迟面板（Get Latency / Put Latency），可取值为 `P50`、`P90`、`P99`、`Avg` |
| **Disk Path** | 按块设备路径筛选，支持多选/全选 | 仅影响 Per-Disk 带宽面板（读/写/总带宽） |

### 4.2 图例说明

所有面板图例以 Table 模式显示在底部，自动计算 **mean**（窗口均值）和 **lastNotNull**（最新非空值）。鼠标悬停时以 multi 模式同时展示所有序列当前值。

- 延迟面板：`{{rank}} @ {{quantile}}`
- 带宽面板：`{{rank}} @ {{operation}}`
- 容量面板：`{{rank}} @ capacity` 和 `{{rank}} @ used` 双曲线对比
- 每盘带宽面板：`{{rank}} @ {{disk_path}}`
- 盘健康面板：`{{rank}} @ total` 和 `{{rank}} @ fault` 双曲线对比

### 4.3 刷新策略

看板默认每 10 秒自动刷新，可通过右上角下拉框调整。

## 5. 性能问题排查指南

### 5.1 延时异常排查

**场景一：Client 传输延时偏高**

1. 打开 **Get Latency** / **Put Latency** 面板（30s Window）
2. 通过 Quantile 下拉框仅勾选 `P99` 和 `Avg`，观察尾延时是否持续走高
3. 通过 Rank 下拉框逐节点勾选，区分全局劣化还是单节点异常
4. P99 显著高于 P50：可能存在偶发大 key 操作或网络抖动
5. 单节点异常：检查该节点网络链路状态或负载

**场景二：所有 Rank 延迟同步升高**

1. 可能原因：
   - Meta 端处理瓶颈导致 Client 等待
   - 网络拥塞（结合带宽面板确认）
2. 交叉验证：检查 Meta 侧延时面板是否同步升高

### 5.2 带宽异常排查

**场景一：带宽突降**

1. 打开 **Get Bandwidth** / **Put Bandwidth** 面板（30s Window）
2. 观察各 Rank 带宽曲线是否同步下降
3. 同步下降的可能原因：
   - 业务流量整体下降
   - 网络拥塞或链路故障
   - Meta 端处理瓶颈导致 Client 等待
4. 单 Rank 下降：检查该 Client 进程是否存活、网络链路是否正常

**场景二：各 Rank 带宽不均衡**

1. 打开 **Get Bandwidth** / **Put Bandwidth** 面板
2. 逐 Rank 对比带宽分布，定位热点 Rank
3. 交叉查看该 Rank 的延迟面板是否同步升高

### 5.3 累计指标异常排查

1. 打开 **Cumulative Latency** / **Cumulative Bytes** 面板
2. 曲线应单调递增。若出现下降或归零，表示 Client 重启
3. 打开 **Cumulative Bandwidth** 面板
4. 生命周期平均带宽持续走低：说明进程启动早期有大流量，后期流量衰减
5. 累计传输量与窗口带宽趋势一致时，系统处于稳态

### 5.4 UBS IO 存储异常排查

**场景一：SSD 使用率持续增长**

1. 打开 **SSD Capacity & Usage** 面板
2. 观察 `used` 曲线增长速率是否异常
3. 可能原因：
   - 写入流量突增，未能及时清理过期数据
   - SSD 盘故障导致可用容量缩减（结合 **Disk Count & Faults** 面板交叉确认）
4. 排查手段：检查对应 Rank 的 PUT 带宽面板是否同步升高；登录对应节点确认 SSD 挂载点实际使用率

**场景二：内存使用率异常偏高**

1. 打开 **Memory Capacity & Usage** 面板
2. 可能原因：WriteCache 内存池配置过小，或存在内存泄漏（观察 `used` 曲线是否单调递增不复位）
3. 排查手段：检查 Client 进程内存占用

**场景三：单盘读写带宽显著低于其他盘**

1. 打开 **Per-Disk Read/Write/Total Bandwidth** 面板
2. 通过 `$disk_path` 下拉框逐盘对比
3. 某盘带宽远低于其他盘 → 疑似硬件劣化（慢盘）

**场景四：盘故障**

1. 打开 **Disk Count & Faults** 面板
2. `fault` 曲线 > 0 表示存在故障盘
3. 故障定位：
   - 通过 `$rank` 筛选器确定故障发生在哪个节点
   - 查看 `memcache_client_ubs_io_disk_status` 指标确定具体故障盘
4. 处理措施：登录对应节点，检查 `/dev/` 下对应块设备状态，必要时更换硬件

## 6. 注意事项

1. **编译要求**：Client 指标采集默认开启，无需额外配置开关。

2. **指标时效性**：
   - Client 端每 30s 采集并上报一次指标快照至 Meta
   - Meta 端超过 90s 未更新的 Client 指标自动标记为 stale 并清理
   - 看板上看到的是最近一个上报周期的聚合值，非实时瞬时值
   - 每盘带宽由 UBS IO 库按 200ms 周期采样，Client 采集时获取最新采样值

3. **上报失败处理**：窗口计数器仅在 RPC SyncCall 成功后 Reset，失败时数据自动累积至下个周期，不丢数据。

4. **Dashboard 兼容性**：看板 JSON 基于 Grafana schema v39，建议使用 Grafana 9.x 及以上版本。
