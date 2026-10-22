# MemCache 异步刷盘与预取特性介绍

## 1. 特性概述

MemCache 三级缓存架构中，DRAM→SSD 刷盘和 SSD→DRAM 回温是数据冷热分层的两条核心路径。本次更新在这两条路径上分别引入了**异步刷盘**和**预取**机制：

- **异步刷盘**：数据写入 DRAM 后，由后台线程按批次异步刷入 SSD，水位控制只用于淘汰，不决定是否刷盘。上层 `put` / `put_batch` 请求不受 SSD 写入延迟影响。
- **预取**：通过 `is_exist` / `batch_is_exist` 接口探测到数据仅在 SSD 时，主动触发 SSD→DRAM 回温。回温完成后，后续 `get` / `get_batch` 可获得接近原生 DRAM 命中的访问时延。

两项特性均依赖 SSD 池化开关（`ock.mmc.local_service.storage.enabled = true`），对上层应用完全透明，无需修改调用代码。

## 2. 工作原理

### 2.1 异步刷盘

数据写入 DRAM 后，后台线程会按批次异步刷入 SSD，`put` / `put_batch` 请求无需等待 SSD 写入完成即可返回。刷盘成功后，SSD 副本会注册到缓存元数据，后续 `get` / `get_batch` 会根据数据所在层级从 DRAM 或 SSD 读取。

### 2.2 预取

预取在 `is_exist` / `batch_is_exist` 接口中触发。当检查发现某个 key 的数据仅存在于 SSD（DRAM/HBM 上无可读副本）时，系统主动将其从 SSD 回温到 DRAM。

批量 `batch_is_exist` 会将所有命中 SSD 的 key 一次性聚合提交，提升回温效率。

预取失败不影响正确性，后续 `get` / `get_batch` 仍可通过被动回温路径获取数据。

## 3. 配置说明

### 3.1 mmc-local.conf

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `ock.mmc.local_service.storage.enabled` | bool | `false` | SSD 缓存总开关，启用后异步刷盘和预取才生效 |

### 3.2 mmc-meta.conf

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `ock.mmc.evict_threshold_high` | int | `90` | DRAM 高水位（%），达到后触发淘汰 |
| `ock.mmc.evict_threshold_low` | int | `80` | 淘汰目标水位（%），淘汰至该水位停止 |

### 3.3 示例配置

```ini
# mmc-local.conf
ock.mmc.local_service.storage.enabled = true

# mmc-meta.conf
ock.mmc.evict_threshold_high = 90
ock.mmc.evict_threshold_low = 80
```

## 4. 指标与观测

通过 MetaService 的 `/metrics/summary` 接口返回单行 `key=value` 摘要，其中与本特性相关的字段如下：

| 字段 | 含义 |
|------|------|
| `rewarm` | 成功完成 SSD→DRAM 回温的数据块数量 |
| `flush_bytes` | 异步刷盘写入 SSD 的总字节数 |
| `get_hit_dram` | `get` / `get_batch` 请求直接从 DRAM 读取的 key 数 |
| `get_hit_ssd` | `get` / `get_batch` 请求命中 SSD 并回温成功后返回的 key 数 |
| `dram_used` | DRAM 已用/总容量 |
| `ssd_used` | SSD 已用/总容量；未配置 SSD 时为 `0/0` |

**验证方法：**

- 预取生效：`rewarm` 持续增长；`get_hit_dram` 占 `get_hit_dram + get_hit_ssd` 的比例上升
- 刷盘生效：`flush_bytes` 持续增长

## 5. 使用示例

推荐用法是先在 `mmc-local.conf` 中启用 SSD 池化，再在 Python 侧配置业务内存并正常读写：

```ini
# mmc-local.conf
ock.mmc.local_service.storage.enabled = true
```

```python
from memcache_hybrid import DistributedObjectStore, LocalConfig

config = LocalConfig()
config.dram_size = "400GB"
store = DistributedObjectStore()
store.setup(config)

# 批量读写，写入 DRAM 后异步刷 SSD，is_exist / batch_is_exist 自动触发预取
keys = ["key1", "key2", "key3"]
values = [b"data" * 1024 for _ in keys]
store.put_batch(keys, values)
data = store.get_batch(keys)
```

## 6. 约束与限制

- 异步刷盘和预取均依赖 `ock.mmc.local_service.storage.enabled = true`，未启用 SSD 时无效
- 预取仅在 `is_exist` / `batch_is_exist` 接口中触发，`get` / `get_batch` 接口不触发预取（只做被动回温）
- 预取失败不影响正确性，后续 `get` / `get_batch` 仍可通过被动回温获取数据

## 7. FAQ

**Q1: 如何确认预取在工作？**

观察 `/metrics/summary` 中 `rewarm` 是否持续增长，同时 `get_hit_dram` 占比是否上升。

**Q2: 刷盘延迟高怎么办？**

调整刷盘相关参数使刷盘更频繁且批量更大。同时确认 SSD 大 IO 随机读带宽满足要求（建议不低于 7GB/s）。

**Q3: `get` 请求偶尔超时？**

等待回温的超时时间通过 `mmc-meta.conf` 中的 `ock.mmc.storage.pending_wait.timeout` 配置，单位为毫秒。建议根据 SSD 读取延迟合理设置：SSD 慢盘场景适当增大；延迟敏感场景可适当减小，超时后降级读 SSD。

**Q4: 预取会不会把 DRAM 打满？**

预取执行前会检查 DRAM 空间，不足时先触发淘汰腾出空间。但如果预取速率持续大于淘汰速率，仍可能导致水位持续上升。建议合理设置 `evict_threshold_high`，确保高水位到 100% 之间的空间能覆盖单节点 NPU HBM KV Cache 总容量。
