# MemCache Hybrid RESTful API 文档

本文档定义 MemCache Hybrid REST 接口的目标契约；如某接口当前未实现，会在对应条目标注状态。

- 默认 HTTP 地址来自配置项 `ock.mmc.meta_service.metrics_url`
- 默认值：`http://127.0.0.1:8000`；host 支持任意 IP 或域名
- 支持通过 `ock.mmc.metrics.tls.*` 配置项独立开启 mTLS
- 成功响应的 `Content-Type` 由各接口单独约定
- 业务错误也返回 `HTTP 200`；客户端需通过响应体中的 `success` 字段判断是否成功
- 错误返回统一使用 JSON 格式；`error_message` 会随具体错误场景变化
- `error_message` 返回实际错误原因；未实现接口等场景可返回 `Not supported`
- 若成功响应包含 `timestamp`，其含义由对应接口定义；错误响应中的 `timestamp` 表示错误响应生成时间，示例中的 `0` 仅为占位值

## 1 `GET /metadata?key=...`

### 作用

按原样读取指定 metadata value；成功时直接返回原始内容，不额外包装 JSON。

### curl

```bash
curl "http://127.0.0.1:8000/metadata?key=demo_key"
```

### 请求参数

| 参数名 | 类型 | 必填 | 描述 |
|---|---|---|---|
| `key` | String | 是 | 要读取的 metadata key |

### 成功示例

`text/plain; charset=utf-8`：

```text
demo metadata value
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| 原样返回 value | 成功时直接返回 metadata value，不重组外层 JSON | 接口契约 |
| 成功 `Content-Type` | 本文示例使用 `text/plain; charset=utf-8` | 示例约定 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 2 `PUT /metadata?key=...`

### 作用

按原始文本写入指定 metadata value。

### curl

```bash
curl -X PUT "http://127.0.0.1:8000/metadata?key=demo_key" \
  --data 'demo metadata value'
```

### 请求参数

| 参数名 | 类型 | 必填 | 描述 |
|---|---|---|---|
| `key` | String | 是 | 要写入的 metadata key |

### 请求体

`key` 对应的原始文本；服务端按收到的 body 原样写入，不做 schema 校验。

请求体建议使用 `Content-Type: text/plain; charset=utf-8`。

### 成功示例

`text/plain; charset=utf-8`：

```text
metadata updated
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| `metadata updated` | metadata 写入成功后的固定文本 | 接口成功返回约定 |
| 原样写入 | 请求体不做 schema 校验，按原文保存 | 接口契约 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 3 `DELETE /metadata?key=...`

### 作用

删除指定 metadata key。

### curl

```bash
curl -X DELETE "http://127.0.0.1:8000/metadata?key=demo_key"
```

### 请求参数

| 参数名 | 类型 | 必填 | 描述 |
|---|---|---|---|
| `key` | String | 是 | 要删除的 metadata key |

### 成功示例

`text/plain; charset=utf-8`：

```text
metadata deleted
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| `metadata deleted` | metadata 删除成功后的固定文本 | 接口成功返回约定 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 4 `GET /health`

### 作用

返回 HTTP 服务健康状态、HA 状态和服务就绪状态。

### curl

```bash
curl "http://127.0.0.1:8000/health"
```

### 请求参数

无

### 成功示例

`application/json; charset=utf-8`：

```json
{
  "status": "ok",
  "role": "leader",
  "ha_state": "serving",
  "service_ready": true,
  "leader_address": "unknown",
  "view_version": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `status` | 健康状态；成功时固定为 `ok` | 接口成功返回约定 |
| `role` | 当前角色，取值 `leader / standby / unknown` | HA 状态快照 |
| `ha_state` | 当前 HA 状态，取值 `starting / standby / serving / unknown` | HA 状态快照 |
| `service_ready` | HTTP 服务是否已准备就绪 | 服务运行状态 |
| `leader_address` | leader 地址；无稳定来源时固定 `unknown` | HA 状态快照 |
| `view_version` | 视图版本；无稳定来源时固定 `0` | HA 状态快照 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 5 `GET /metrics`

### 作用

以 Prometheus 文本格式导出 MemCache 监控指标。当前无法提供的字段允许以 `0` 或 `false` 等占位值导出。

### curl

```bash
curl "http://127.0.0.1:8000/metrics"
```

### 请求参数

无

### 成功示例

`text/plain; version=0.0.4`：

```text
# HELP memcache_segment_capacity_bytes Segment total capacity in bytes
# TYPE memcache_segment_capacity_bytes gauge
memcache_segment_capacity_bytes{segment="rank-0-hbm"} 5368709120
# HELP memcache_segment_allocated_bytes Segment allocated bytes
# TYPE memcache_segment_allocated_bytes gauge
memcache_segment_allocated_bytes{segment="rank-0-hbm"} 368640
# HELP memcache_total_capacity_bytes Total capacity by medium in bytes
# TYPE memcache_total_capacity_bytes gauge
memcache_total_capacity_bytes{medium="hbm"} 5368709120
# HELP memcache_allocated_bytes Allocated bytes by medium
# TYPE memcache_allocated_bytes gauge
memcache_allocated_bytes{medium="hbm"} 368640
# HELP memcache_get_requests_total Total number of Get requests
# TYPE memcache_get_requests_total counter
memcache_get_requests_total 68
# HELP memcache_get_successes_total Total number of Get successes
# TYPE memcache_get_successes_total counter
memcache_get_successes_total 68
# HELP memcache_get_failures_total Total number of Get failures
# TYPE memcache_get_failures_total counter
memcache_get_failures_total 0
# HELP memcache_get_not_found_total Total number of Get not found results
# TYPE memcache_get_not_found_total counter
memcache_get_not_found_total 0
# HELP memcache_evict_operations_total Total number of eviction operations
# TYPE memcache_evict_operations_total counter
memcache_evict_operations_total 0
# HELP memcache_evict_to_ssd_total Total number of eviction to SSD operations
# TYPE memcache_evict_to_ssd_total counter
memcache_evict_to_ssd_total 0
# HELP memcache_evict_ssd_delete_total Total number of SSD eviction delete operations
# TYPE memcache_evict_ssd_delete_total counter
memcache_evict_ssd_delete_total 0
# HELP memcache_evict_mem_delete_total Total number of memory tier eviction delete operations
# TYPE memcache_evict_mem_delete_total counter
memcache_evict_mem_delete_total 0
# HELP memcache_evict_running Whether eviction (GC) is currently running (1=yes, 0=no)
# TYPE memcache_evict_running gauge
memcache_evict_running 0
# HELP memcache_get_hits_dram_total Total number of Get hits served from DRAM
# TYPE memcache_get_hits_dram_total counter
memcache_get_hits_dram_total 0
# HELP memcache_get_hits_ssd_total Total number of Get hits served from SSD (rewarm)
# TYPE memcache_get_hits_ssd_total counter
memcache_get_hits_ssd_total 0
# HELP memcache_rewarm_total Total number of SSD->DRAM rewarm operations
# TYPE memcache_rewarm_total counter
memcache_rewarm_total 0
# HELP memcache_rewarm_failed_total Total number of failed SSD->DRAM rewarm operations
# TYPE memcache_rewarm_failed_total counter
memcache_rewarm_failed_total 0
# HELP memcache_rewarm_bytes_total Total bytes rewarmed from SSD to DRAM
# TYPE memcache_rewarm_bytes_total counter
memcache_rewarm_bytes_total 0
# HELP memcache_rewarm_bytes_current Current bytes occupied by rewarmed data
# TYPE memcache_rewarm_bytes_current gauge
memcache_rewarm_bytes_current 0
# HELP memcache_stored_keys Current number of stored keys
# TYPE memcache_stored_keys gauge
memcache_stored_keys 2
# HELP memcache_kv_events_published_total Total KV cache events published
# TYPE memcache_kv_events_published_total counter
memcache_kv_events_published_total 0
# HELP memcache_kv_events_published_by_type_total Total KV cache events published by event type
# TYPE memcache_kv_events_published_by_type_total counter
memcache_kv_events_published_by_type_total{type="stored"} 0
# HELP memcache_kv_events_published_by_medium_total Total KV cache events published by storage medium
# TYPE memcache_kv_events_published_by_medium_total counter
memcache_kv_events_published_by_medium_total{medium="hbm"} 0
# HELP memcache_kv_events_dropped_total Total KV cache events dropped on queue pressure
# TYPE memcache_kv_events_dropped_total counter
memcache_kv_events_dropped_total 0
# HELP memcache_kv_events_dropped_stored_total Total KV cache stored events dropped on full queue
# TYPE memcache_kv_events_dropped_stored_total counter
memcache_kv_events_dropped_stored_total 0
# HELP memcache_kv_events_dropped_high_priority_total Total KV cache removed or cleared events dropped after hard queue limit
# TYPE memcache_kv_events_dropped_high_priority_total counter
memcache_kv_events_dropped_high_priority_total 0
# HELP memcache_kv_events_skipped_unparsed_total Total KV cache events that could not derive block_hashes from object key
# TYPE memcache_kv_events_skipped_unparsed_total counter
memcache_kv_events_skipped_unparsed_total 0
# HELP memcache_kv_events_queue_size Current KV cache event queue size
# TYPE memcache_kv_events_queue_size gauge
memcache_kv_events_queue_size 0
# HELP memcache_kv_events_queue_capacity Configured KV cache event soft queue capacity
# TYPE memcache_kv_events_queue_capacity gauge
memcache_kv_events_queue_capacity 65536
# HELP memcache_kv_events_publisher_active Whether KV event publishing is active
# TYPE memcache_kv_events_publisher_active gauge
memcache_kv_events_publisher_active 1
# HELP memcache_kv_events_last_sequence Last published KV event ZMQ sequence
# TYPE memcache_kv_events_last_sequence gauge
memcache_kv_events_last_sequence 0
# HELP memcache_metric_stale Client metric stale status (1 = stale, 0 = fresh)
# TYPE memcache_metric_stale gauge
memcache_metric_stale{rank="0"} 0
# HELP memcache_bandwidth_bytes Client bandwidth window bytes transferred
# TYPE memcache_bandwidth_bytes gauge
memcache_bandwidth_bytes{rank="0",operation="batch_put"} 0
# HELP memcache_bandwidth_duration_ms Client bandwidth window duration in milliseconds
# TYPE memcache_bandwidth_duration_ms gauge
memcache_bandwidth_duration_ms{rank="0",operation="batch_put"} 0
# HELP memcache_bandwidth_bytes_cum_total Client bandwidth cumulative bytes transferred
# TYPE memcache_bandwidth_bytes_cum_total counter
memcache_bandwidth_bytes_cum_total{rank="0",operation="batch_put"} 0
# HELP memcache_bandwidth_duration_ms_cum_total Client bandwidth cumulative duration in milliseconds
# TYPE memcache_bandwidth_duration_ms_cum_total counter
memcache_bandwidth_duration_ms_cum_total{rank="0",operation="batch_put"} 0
# HELP memcache_bandwidth_latency_seconds Client bandwidth operation latency in seconds
# TYPE memcache_bandwidth_latency_seconds gauge
memcache_bandwidth_latency_seconds{rank="0",operation="batch_put",quantile="P50"} 0
# HELP memcache_bandwidth_bytes_per_sec Client bandwidth instantaneous bytes per second
# TYPE memcache_bandwidth_bytes_per_sec gauge
memcache_bandwidth_bytes_per_sec{rank="0",operation="batch_put"} 0
# HELP memcache_ubs_io_disk_capacity_bytes Client UBS IO SSD total capacity in bytes
# TYPE memcache_ubs_io_disk_capacity_bytes gauge
memcache_ubs_io_disk_capacity_bytes{rank="0"} 0
# HELP memcache_ubs_io_disk_used_bytes Client UBS IO SSD used bytes
# TYPE memcache_ubs_io_disk_used_bytes gauge
memcache_ubs_io_disk_used_bytes{rank="0"} 0
# HELP memcache_ubs_io_mem_capacity_bytes Client UBS IO memory total capacity in bytes
# TYPE memcache_ubs_io_mem_capacity_bytes gauge
memcache_ubs_io_mem_capacity_bytes{rank="0"} 0
# HELP memcache_ubs_io_mem_used_bytes Client UBS IO memory used bytes
# TYPE memcache_ubs_io_mem_used_bytes gauge
memcache_ubs_io_mem_used_bytes{rank="0"} 0
# HELP memcache_ubs_io_disk_num Client UBS IO total disk count
# TYPE memcache_ubs_io_disk_num gauge
memcache_ubs_io_disk_num{rank="0"} 0
# HELP memcache_ubs_io_fault_disk_num Client UBS IO fault disk count
# TYPE memcache_ubs_io_fault_disk_num gauge
memcache_ubs_io_fault_disk_num{rank="0"} 0
# HELP memcache_ubs_io_disk_status Client UBS IO per-disk status (0=normal, non-zero=fault)
# TYPE memcache_ubs_io_disk_status gauge
memcache_ubs_io_disk_status{rank="0",disk_path="/data/disk0"} 0
# HELP memcache_ubs_io_disk_read_bandwidth_bytes_per_sec Client UBS IO per-disk read bandwidth
# TYPE memcache_ubs_io_disk_read_bandwidth_bytes_per_sec gauge
memcache_ubs_io_disk_read_bandwidth_bytes_per_sec{rank="0",disk_path="/data/disk0"} 0
# HELP memcache_ubs_io_disk_write_bandwidth_bytes_per_sec Client UBS IO per-disk write bandwidth
# TYPE memcache_ubs_io_disk_write_bandwidth_bytes_per_sec gauge
memcache_ubs_io_disk_write_bandwidth_bytes_per_sec{rank="0",disk_path="/data/disk0"} 0
# HELP memcache_ubs_io_disk_total_bandwidth_bytes_per_sec Client UBS IO per-disk total bandwidth
# TYPE memcache_ubs_io_disk_total_bandwidth_bytes_per_sec gauge
memcache_ubs_io_disk_total_bandwidth_bytes_per_sec{rank="0",disk_path="/data/disk0"} 0
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| Prometheus 文本 | 成功时返回 Prometheus exposition 文本 | 接口成功返回约定 |
| 成功示例 | 成功示例以每个指标一行代表值覆盖大部分指标与标签字段，非逐行完整样例；标签取值见下文指标说明表 | 监控指标契约 |
| 输出顺序 | 指标按分组输出，主要分组顺序为：容量 → 业务调用 → 驱逐/回温/命中 → `stored_keys` → KV 事件 → 客户端指标 | 监控指标契约 |
| 结果口径 | `successes_total` 表示 `MMC_OK`；`failures_total` 表示真实错误（包括 `MMC_DUPLICATED_OBJECT`）；`not_found_total` 表示 `MMC_UNMATCHED_KEY`，不计入 failure；Batch 指标保留接口调用级统计，Batch 子项同时累计到对应非 Batch 指标；Batch 调用级统计中真实错误优先，只有无真实错误且存在子项 `MMC_UNMATCHED_KEY` 时才增加 Batch `not_found_total` | 监控指标契约 |
| 占位值策略 | 当前无法提供真实值的指标仍保留在成功体中，可使用 `0` 或 `false` 占位 | 降级规则 |

**指标说明**

下表按输出顺序逐项说明。

| 指标 | 类型 | 标签 | 数据来源 | 含义 |
|---|---|---|---|---|
| `memcache_segment_capacity_bytes` | gauge | `segment` | `RestSegmentSnapshot::totalBytes` | segment 总容量 |
| `memcache_segment_allocated_bytes` | gauge | `segment` | `RestSegmentSnapshot::usedBytes` | segment 已分配容量 |
| `memcache_total_capacity_bytes` | gauge | `medium`=`hbm`\|`dram`\|`ssd` | `RestUsageSnapshot::totalBytes` | 按介质汇总总容量；`ssd` 仅在容量或已用量非 0 时输出 |
| `memcache_allocated_bytes` | gauge | `medium`=`hbm`\|`dram`\|`ssd` | `RestUsageSnapshot::usedBytes` | 按介质汇总已分配容量；`ssd` 同上 |
| `memcache_<op>_requests_total` | counter | — | `MmcMetaMetricSnapshot::<op>RequestCount` | `<op>` 接口调用总次数；`<op>` ∈ {alloc, batch_alloc, get, batch_get, remove, batch_remove, remove_all, update_state, batch_update_state, query, batch_query, get_all_keys, exist_key, batch_exist_key, mount, unmount}；支持 per-rank（开启 `IsPerRankEnabled()` 时另输出同名 `{rank}` 标签明细） |
| `memcache_<op>_successes_total` | counter | — | `MmcMetaMetricSnapshot::<op>SuccessCount` | `<op>` 成功次数（`MMC_OK`）；支持 per-rank |
| `memcache_<op>_failures_total` | counter | — | `MmcMetaMetricSnapshot::<op>FailureCount` | `<op>` 失败次数（真实错误，含 `MMC_DUPLICATED_OBJECT`）；支持 per-rank |
| `memcache_<op>_not_found_total` | counter | — | `MmcMetaMetricSnapshot::<op>NotFoundCount` | `<op>` 未命中次数（`MMC_UNMATCHED_KEY`），不计入 failure；仅 `get`/`batch_get`/`remove`/`batch_remove`/`update_state`/`batch_update_state`/`query`/`batch_query`/`exist_key`/`batch_exist_key` 输出；支持 per-rank |
| `memcache_evict_operations_total` | counter | — | `MmcMetaMetricSnapshot::evictCount` | 驱逐操作总数；支持 per-rank |
| `memcache_evict_to_ssd_total` | counter | — | `MmcMetaMetricSnapshot::evictToSsdCount` | 迁移到 SSD 的驱逐次数；支持 per-rank |
| `memcache_evict_ssd_delete_total` | counter | — | `MmcMetaMetricSnapshot::evictSsdDeleteCount` | 驱逐时 SSD blob 删除数；支持 per-rank |
| `memcache_evict_mem_delete_total` | counter | — | `MmcMetaMetricSnapshot::evictMemDeleteCount` | 驱逐时内存层 blob 删除数；支持 per-rank |
| `memcache_evict_running` | gauge | — | `MmcMetaManager::evictCheck_` | 是否正在执行淘汰(GC)（1=是，0=否） |
| `memcache_get_hits_dram_total` | counter | — | `MmcMetaMetricSnapshot::getHitDramCount` | 命中 DRAM 的 Get 次数；支持 per-rank |
| `memcache_get_hits_ssd_total` | counter | — | `MmcMetaMetricSnapshot::getHitSsdCount` | 命中 SSD（触发回温）的 Get 次数；支持 per-rank |
| `memcache_rewarm_total` | counter | — | `MmcMetaMetricSnapshot::rewarmCount` | SSD→DRAM 回温操作总数；支持 per-rank |
| `memcache_rewarm_failed_total` | counter | — | `MmcMetaMetricSnapshot::rewarmFailCount` | 失败的回温操作数；支持 per-rank |
| `memcache_rewarm_bytes_total` | counter | — | `MmcMetaMetricSnapshot::rewarmBytesCount` | 累计回温字节数；支持 per-rank |
| `memcache_rewarm_bytes_current` | gauge | — | `MmcMetaMetricSnapshot::rewarmBytesCurrent` | 当前在途回温字节数；支持 per-rank |
| `memcache_stored_keys` | gauge | — | `MmcMetaMetricSnapshot::keyCount` | 当前已存储 key 数 |
| `memcache_kv_events_published_total` | counter | — | `KvEventStats::publishedEvents` | 已发布 KV 事件总数 |
| `memcache_kv_events_published_by_type_total` | counter | `type`=`stored`\|`removed`\|`cleared` | `KvEventStats::publishedStoredEvents` 等 | 按事件类型分组的发布数 |
| `memcache_kv_events_published_by_medium_total` | counter | `medium`=`hbm`\|`dram`\|`ssd`\|`unknown` | `KvEventStats::publishedHbmEvents` 等 | 按存储介质分组的发布数 |
| `memcache_kv_events_dropped_total` | counter | — | `KvEventStats::droppedEvents` | 队列满丢弃事件总数 |
| `memcache_kv_events_dropped_stored_total` | counter | — | `KvEventStats::droppedStoredEvents` | 丢弃的 stored 类型事件数 |
| `memcache_kv_events_dropped_high_priority_total` | counter | — | `KvEventStats::droppedHighPriorityEvents` | 丢弃的高优先级（removed/cleared）事件数 |
| `memcache_kv_events_skipped_unparsed_total` | counter | — | `KvEventStats::skippedUnparsedKeys` | 无法解析为 hash 的 key 数 |
| `memcache_kv_events_queue_size` | gauge | — | `KvEventStats::queueSize` | 当前队列事件数 |
| `memcache_kv_events_queue_capacity` | gauge | — | `KvEventStats::queueCapacity` | 队列软容量上限（配置值） |
| `memcache_kv_events_publisher_active` | gauge | — | `KvEventStats::publisherActive` | 发布线程是否运行（1/0） |
| `memcache_kv_events_last_sequence` | gauge | — | `KvEventStats::lastSequence` | 最后发布的 ZMQ 序列号 |
| `memcache_metric_stale` | gauge | `rank` | `RankMetricView::stale` | 客户端 metric 是否过期（1=过期，0=在线）；客户端指标仅在存在在线 `RankMetricView` 时输出 |
| `memcache_bandwidth_bytes` | gauge | `rank`,`operation`=`batch_put`\|`batch_get` | `BandwidthMetricData::totalBytes` | 带宽窗口传输字节数（滑动窗口值） |
| `memcache_bandwidth_duration_ms` | gauge | `rank`,`operation`=`batch_put`\|`batch_get`\|`batch_exist` | `BandwidthMetricData::totalDurationMs` | 带宽窗口时长（ms，滑动窗口值） |
| `memcache_bandwidth_bytes_cum_total` | counter | `rank`,`operation`=`batch_put`\|`batch_get` | `BandwidthMetricData::cumTotalBytes` | 累计传输字节数（自启动起累计） |
| `memcache_bandwidth_duration_ms_cum_total` | counter | `rank`,`operation`=`batch_put`\|`batch_get`\|`batch_exist` | `BandwidthMetricData::cumTotalDurationMs` | 累计时长（ms，自启动起累计） |
| `memcache_bandwidth_latency_seconds` | gauge | `rank`,`operation`=`batch_put`\|`batch_get`\|`batch_exist`,`quantile`=`P50`\|`P90`\|`P99`\|`Avg` | `BandwidthMetricData::latencyP50`/`P90`/`P99`/`Avg` | 操作延迟分位（秒） |
| `memcache_bandwidth_bytes_per_sec` | gauge | `rank`,`operation`=`batch_put`\|`batch_get` | `BandwidthMetricData::bytesPerSec` | 瞬时带宽（B/s） |
| `memcache_ubs_io_disk_capacity_bytes` | gauge | `rank` | `UbsIoMetricData::diskCap` | SSD 总容量 |
| `memcache_ubs_io_disk_used_bytes` | gauge | `rank` | `UbsIoMetricData::diskUsed` | SSD 已用容量 |
| `memcache_ubs_io_mem_capacity_bytes` | gauge | `rank` | `UbsIoMetricData::memCap` | 内存总容量 |
| `memcache_ubs_io_mem_used_bytes` | gauge | `rank` | `UbsIoMetricData::memUsed` | 内存已用容量 |
| `memcache_ubs_io_disk_num` | gauge | `rank` | `UbsIoMetricData::diskNum` | 磁盘总数 |
| `memcache_ubs_io_fault_disk_num` | gauge | `rank` | `UbsIoMetricData::faultDiskNum` | 故障磁盘数 |
| `memcache_ubs_io_disk_status` | gauge | `rank`,`disk_path` | `UbsIoPerDiskMetric::status` | 单盘状态（0=正常） |
| `memcache_ubs_io_disk_read_bandwidth_bytes_per_sec` | gauge | `rank`,`disk_path` | `UbsIoPerDiskMetric::readBandwidth` | 单盘读带宽 |
| `memcache_ubs_io_disk_write_bandwidth_bytes_per_sec` | gauge | `rank`,`disk_path` | `UbsIoPerDiskMetric::writeBandwidth` | 单盘写带宽 |
| `memcache_ubs_io_disk_total_bandwidth_bytes_per_sec` | gauge | `rank`,`disk_path` | `UbsIoPerDiskMetric::totalBandwidth` | 单盘总带宽 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 6 `GET /metrics/summary`

### 作用

返回固定字段顺序的单行文本摘要。属于统计汇总接口，当前无法提供的字段允许按降级策略返回占位值。

返回格式为单行文本，使用空格分隔的 `key=value` 片段组成；字段顺序固定，不换行，不做 JSON 包装。

### curl

```bash
curl "http://127.0.0.1:8000/metrics/summary"
```

### 请求参数

无

### 成功示例

`text/plain; charset=utf-8`：

```text
keys=2 evict=0 evict_to_ssd=0 evict_ssd_delete=0 evict_mem_delete=0 rewarm=0 rewarm_fail=0 rewarm_bytes_total=0 rewarm_bytes_current=0 get_hit_dram=0 get_hit_ssd=0 hbm_used=368640/5368709120 dram_used=0/5368709120 ssd_used=0/0 alloc_req=68 alloc_success=68 alloc_fail=0 batch_alloc_req=0 batch_alloc_success=0 batch_alloc_fail=0 get_req=68 get_success=68 get_fail=0 get_not_found=0 batch_get_req=0 batch_get_success=0 batch_get_fail=0 batch_get_not_found=0 remove_req=0 remove_success=0 remove_fail=0 remove_not_found=0 batch_remove_req=0 batch_remove_success=0 batch_remove_fail=0 batch_remove_not_found=0 remove_all_req=0 remove_all_success=0 remove_all_fail=0 update_state_req=0 update_state_success=0 update_state_fail=0 update_state_not_found=0 batch_update_state_req=0 batch_update_state_success=0 batch_update_state_fail=0 batch_update_state_not_found=0 query_req=12 query_success=11 query_fail=0 query_not_found=1 batch_query_req=3 batch_query_success=3 batch_query_fail=0 batch_query_not_found=1 get_all_keys_req=4 get_all_keys_success=4 get_all_keys_fail=0 exist_key_req=0 exist_key_success=0 exist_key_fail=0 exist_key_not_found=0 batch_exist_key_req=0 batch_exist_key_success=0 batch_exist_key_fail=0 batch_exist_key_not_found=0 mount_req=0 mount_success=0 mount_fail=0 unmount_req=0 unmount_success=0 unmount_fail=0
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| 单行文本 | 成功时必须为单行，不能换行拆分 | 接口成功返回约定 |
| 格式 | 以空格分隔的 `key=value` 串，字段顺序固定 | 统计摘要接口契约 |
| 成功示例字段 | 成功示例应包含当前约定的全部字段 | 统计摘要接口契约 |
| 字段顺序固定 | 依次为 `keys`、`evict`、`evict_to_ssd`、`evict_ssd_delete`、`evict_mem_delete`、`rewarm`、`rewarm_fail`、`rewarm_bytes_total`、`rewarm_bytes_current`、`get_hit_dram`、`get_hit_ssd`、`hbm_used`、`dram_used`、`ssd_used`、`alloc_req`、`alloc_success`、`alloc_fail`、`batch_alloc_req`、`batch_alloc_success`、`batch_alloc_fail`、`get_req`、`get_success`、`get_fail`、`get_not_found`、`batch_get_req`、`batch_get_success`、`batch_get_fail`、`batch_get_not_found`、`remove_req`、`remove_success`、`remove_fail`、`remove_not_found`、`batch_remove_req`、`batch_remove_success`、`batch_remove_fail`、`batch_remove_not_found`、`remove_all_req`、`remove_all_success`、`remove_all_fail`、`update_state_req`、`update_state_success`、`update_state_fail`、`update_state_not_found`、`batch_update_state_req`、`batch_update_state_success`、`batch_update_state_fail`、`batch_update_state_not_found`、`query_req`、`query_success`、`query_fail`、`query_not_found`、`batch_query_req`、`batch_query_success`、`batch_query_fail`、`batch_query_not_found`、`get_all_keys_req`、`get_all_keys_success`、`get_all_keys_fail`、`exist_key_req`、`exist_key_success`、`exist_key_fail`、`exist_key_not_found`、`batch_exist_key_req`、`batch_exist_key_success`、`batch_exist_key_fail`、`batch_exist_key_not_found`、`mount_req`、`mount_success`、`mount_fail`、`unmount_req`、`unmount_success`、`unmount_fail` | 统计摘要接口契约 |
| 容量字段 | `hbm_used` / `dram_used` / `ssd_used` 格式为 `used/total`；未配置 SSD 时 `ssd_used` 为 `0/0` | 统计摘要接口契约 |
| per-rank 字段 | 开启 `MmcMetaMetricManager::IsPerRankEnabled()` 时，在上述固定字段后以 `<base>_by_rank_<rank>=<val>` 形式追加 per-rank 字段（每个 rank 一项），覆盖驱逐/回温/命中 10 项：`evict_by_rank`、`evict_to_ssd_by_rank`、`evict_ssd_delete_by_rank`、`evict_mem_delete_by_rank`、`get_hit_dram_by_rank`、`get_hit_ssd_by_rank`、`rewarm_by_rank`、`rewarm_fail_by_rank`、`rewarm_bytes_by_rank`、`rewarm_bytes_current_by_rank`；业务调用、容量、`keys` 等不输出 per-rank 字段 | per-rank 统计契约 |
| 结果口径 | `*_success` 表示 `MMC_OK`；`*_fail` 表示真实错误（包括 `MMC_DUPLICATED_OBJECT`）；`*_not_found` 表示 `MMC_UNMATCHED_KEY`，不计入 fail；Batch 字段为接口调用级统计，Batch 子项同时累计到对应非 Batch 字段；Batch 调用级统计中真实错误优先，只有无真实错误且存在子项 `MMC_UNMATCHED_KEY` 时才增加 Batch `*_not_found` | 统计摘要接口契约 |
| 占位值策略 | 当前无法提供真实值的字段仍保留在成功体中，可使用 `0`、`0/0` 或空值占位 | 降级规则 |

**字段说明**

下表按输出顺序逐字段说明。

| 字段 | 格式 | 数据来源 | 含义 |
|---|---|---|---|
| `keys` | uint | `MmcMetaMetricSnapshot::keyCount` | 当前已存储 key 数量 |
| `evict` | uint | `MmcMetaMetricSnapshot::evictCount` | 驱逐操作总数 |
| `evict_to_ssd` | uint | `MmcMetaMetricSnapshot::evictToSsdCount` | 迁移到 SSD 的驱逐次数 |
| `evict_ssd_delete` | uint | `MmcMetaMetricSnapshot::evictSsdDeleteCount` | 驱逐时 SSD blob 删除数 |
| `evict_mem_delete` | uint | `MmcMetaMetricSnapshot::evictMemDeleteCount` | 驱逐时内存层 blob 删除数 |
| `rewarm` | uint | `MmcMetaMetricSnapshot::rewarmCount` | SSD→DRAM 回温操作总数 |
| `rewarm_fail` | uint | `MmcMetaMetricSnapshot::rewarmFailCount` | 失败的回温操作数 |
| `rewarm_bytes_total` | uint (bytes) | `MmcMetaMetricSnapshot::rewarmBytesCount` | 累计回温字节数 |
| `rewarm_bytes_current` | uint (bytes) | `MmcMetaMetricSnapshot::rewarmBytesCurrent` | 当前在途回温字节数 |
| `get_hit_dram` | uint | `MmcMetaMetricSnapshot::getHitDramCount` | 命中 DRAM 的 Get 次数 |
| `get_hit_ssd` | uint | `MmcMetaMetricSnapshot::getHitSsdCount` | 命中 SSD（触发回温）的 Get 次数 |
| `hbm_used` | `used/total` (bytes) | `RestUsageSnapshot`(hbm) | HBM 已用/总容量 |
| `dram_used` | `used/total` (bytes) | `RestUsageSnapshot`(dram) | DRAM 已用/总容量 |
| `ssd_used` | `used/total` (bytes) | `RestUsageSnapshot`(ssd) | SSD 已用/总容量；未配置 SSD 时为 `0/0` |
| `<op>_req` | uint | `MmcMetaMetricSnapshot::<op>RequestCount` | `<op>` 接口调用总次数 |
| `<op>_success` | uint | `MmcMetaMetricSnapshot::<op>SuccessCount` | `<op>` 成功次数（`MMC_OK`） |
| `<op>_fail` | uint | `MmcMetaMetricSnapshot::<op>FailureCount` | `<op>` 失败次数（真实错误，含 `MMC_DUPLICATED_OBJECT`） |
| `<op>_not_found` | uint | `MmcMetaMetricSnapshot::<op>NotFoundCount` | `<op>` 未命中次数（`MMC_UNMATCHED_KEY`），不计入 fail |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 7 `GET /metrics/ptracer`

### 作用

导出当前 ptracer 原始文本输出。

### curl

```bash
curl "http://127.0.0.1:8000/metrics/ptracer"
```

### 请求参数

无

### 成功示例

`text/plain; charset=utf-8`：

```text
TIME                   NAME                                    BEGIN          GOOD_END       BAD_END        ON_FLY         P50(us)        P99(us)        P999(us)       AVG(us)        MAX(us)
2026-01-05 14:57:03    TP_MMC_META_PUT                         8              8              0              0              31.880         110.250        131.870        59.379         131.870
2026-01-05 14:57:03    TP_MMC_META_GET                         8              8              0              0              14.820         55.000         62.020         31.398         62.020
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| ptracer 文本 | 成功时保持当前 ptracer 文本输出格式 | 接口成功返回约定 |
| alloc/free 打点 | 如新增 alloc/free 打点，也通过该接口暴露 | ptracer 输出范围 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 8 `GET /role`

### 作用

返回当前角色文本。

### curl

```bash
curl "http://127.0.0.1:8000/role"
```

### 请求参数

无

### 成功示例

`text/plain; charset=utf-8`：

```text
leader
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| `leader` / `standby` / `unknown` | 当前角色文本 | HA 状态映射结果 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 9 `GET /ha_status`

### 作用

返回当前 HA 状态文本。无法稳定映射时返回 `unknown`。

### curl

```bash
curl "http://127.0.0.1:8000/ha_status"
```

### 请求参数

无

### 成功示例

`text/plain; charset=utf-8`：

```text
serving
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| `starting` / `standby` / `serving` / `unknown` | 当前 HA 状态文本 | HA 状态快照 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 10 `GET /leader`

### 作用

返回 leader 是否存在及其地址和视图版本。响应中不包含 `role` 字段；当前无法提供稳定值的字段允许返回默认值。

### curl

```bash
curl "http://127.0.0.1:8000/leader"
```

### 请求参数

无

### 成功示例

`application/json; charset=utf-8`：

```json
{
  "present": true,
  "leader_address": "unknown",
  "view_version": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `present` | 当前是否能确认 leader 存在 | HA 状态快照 |
| `leader_address` | leader 地址；无稳定来源时固定 `unknown` | HA 状态快照 |
| `view_version` | 视图版本；无稳定来源时固定 `0` | HA 状态快照 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 11 `GET /query_key?key=...`

### 作用

查询单个 key 的元数据信息，包括对象大小、访问属性和 blob 分布信息。

### curl

```bash
curl "http://127.0.0.1:8000/query_key?key=key_a"
```

### 请求参数

| 参数名 | 类型 | 必填 | 描述 |
|---|---|---|---|
| `key` | String | 是 | 要查询的键名 |

### 成功示例

`application/json; charset=utf-8`：

```json
{
  "key": "key_a",
  "size": 100,
  "prot": 0,
  "numBlobs": 2,
  "valid": true,
  "blobs": [
    {
      "rank": 0,
      "medium": "HBM",
      "gva": 0,
      "state": 0,
      "leaseTimeoutTtlMs": 0
    },
    {
      "rank": 1,
      "medium": "DRAM",
      "gva": 0,
      "state": 0,
      "leaseTimeoutTtlMs": 0
    }
  ]
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `key` | 当前查询的 key | HTTP 接口返回字段 |
| `size` | 对象总大小 | `MemObjQueryInfo::size_` |
| `prot` | 对象访问属性 | `MemObjQueryInfo::prot_` |
| `numBlobs` | 当前对象对应的 blob 数量 | `MemObjQueryInfo::numBlobs_` |
| `valid` | 当前 key 是否有效 | `MemObjQueryInfo::valid_` |
| `blobs` | blob 描述信息列表 | `MemObjQueryInfo::blobs_` |
| `blobs[].rank` | blob 所在 rank | `MmcMemBlobDesc::rank_` |
| `blobs[].medium` | blob 所在介质类型字符串 | `MmcMemBlobDesc::mediaType_` |
| `blobs[].gva` | blob 的全局虚拟地址 | `MmcMemBlobDesc::gva_` |
| `blobs[].state` | blob 在 meta 侧的状态 | `MmcMemBlobDesc::state_` |
| `blobs[].leaseTimeoutTtlMs` | 剩余读租约 TTL，单位毫秒 | `MmcMemBlobDesc::leaseTimeoutTtlMs_` |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 12 `GET /batch_query_keys?keys=...`

### 作用

批量查询多个 key 的元数据信息；单个 key 的字段定义与 `/query_key` 保持一致。

### curl

```bash
curl "http://127.0.0.1:8000/batch_query_keys?keys=key_a,key_b"
```

### 请求参数

| 参数名 | 类型 | 必填 | 描述 |
|---|---|---|---|
| `keys` | String | 是 | 逗号分隔的 key 列表 |

### 成功示例

`application/json; charset=utf-8`：

```json
{
  "success": true,
  "data": [
    {
      "key": "key_a",
      "size": 100,
      "prot": 0,
      "numBlobs": 1,
      "valid": true,
      "blobs": [
        {
          "rank": 0,
          "medium": "HBM",
          "gva": 0,
          "state": 0,
          "leaseTimeoutTtlMs": 0
        }
      ]
    },
    {
      "key": "key_b",
      "size": 0,
      "prot": 0,
      "numBlobs": 0,
      "valid": false,
      "blobs": []
    }
  ]
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 接口级调用是否成功；成功示例中为 `true` | 成功体契约 |
| `data` | 按请求顺序返回的查询结果数组 | 成功体契约 |
| `data[].key` | 当前结果对应的 key | 与 `/query_key` 保持一致 |
| `data[].size` | 对象总大小 | 与 `/query_key` 保持一致 |
| `data[].prot` | 对象访问属性 | 与 `/query_key` 保持一致 |
| `data[].numBlobs` | 当前对象对应的 blob 数量 | 与 `/query_key` 保持一致 |
| `data[].valid` | 当前 key 是否有效 | 与 `/query_key` 保持一致 |
| `data[].blobs` | blob 描述信息列表 | 与 `/query_key` 保持一致 |
| `data[].blobs[].rank` | blob 所在 rank | 与 `/query_key` 保持一致 |
| `data[].blobs[].medium` | blob 所在介质类型字符串 | 与 `/query_key` 保持一致 |
| `data[].blobs[].gva` | blob 的全局虚拟地址 | 与 `/query_key` 保持一致 |
| `data[].blobs[].state` | blob 在 meta 侧的状态 | 与 `/query_key` 保持一致 |
| `data[].blobs[].leaseTimeoutTtlMs` | 剩余读租约 TTL，单位毫秒 | 与 `/query_key` 保持一致 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 接口级请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 13 `DELETE /key?key=...`

### 作用

删除指定业务 KVCache key 及其关联数据。

### curl

```bash
curl -X DELETE "http://127.0.0.1:8000/key?key=demo_key"
```

### 请求参数

| 参数名 | 类型 | 必填 | 描述 |
|---|---|---|---|
| `key` | String | 是 | 要删除的业务 KVCache key |

### 成功示例

`application/json; charset=utf-8`：

```json
{
  "success": true,
  "message": "key deleted"
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；成功场景固定为 `true` | 接口成功返回约定 |
| `message` | 成功提示信息 | 接口成功返回约定 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 14 `DELETE /all_keys`

### 作用

删除所有业务 KVCache key 及其关联数据。

### curl

```bash
curl -X DELETE "http://127.0.0.1:8000/all_keys"
```

### 请求参数

无

### 成功示例

`application/json; charset=utf-8`：

```json
{
  "success": true,
  "message": "all keys deleted"
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；成功场景固定为 `true` | 接口成功返回约定 |
| `message` | 成功提示信息 | 接口成功返回约定 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 15 `GET /get_all_keys`

### 作用

列出全部对象 key 列表。

### curl

```bash
curl "http://127.0.0.1:8000/get_all_keys"
```

### 请求参数

无

### 成功示例

`text/plain; charset=utf-8`：

```text
key_1
key_2
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| 每行一个 key | 返回 key 列表；逐行拼接，末尾可带换行；列表为空时 body 也可为空 | 接口成功返回约定 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 16 `GET /get_all_segments`

### 作用

列出全部 `segment_id`。当前版本逐行返回文本，不返回 JSON 数组。

### curl

```bash
curl "http://127.0.0.1:8000/get_all_segments"
```

### 请求参数

无

### 成功示例

`text/plain; charset=utf-8`：

```text
rank-0-hbm
rank-1-hbm
rank-0-dram
rank-1-dram
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| 每行一个 `segment_id` | 成功时逐行返回 `segment_id` 文本 | 固定成功体形态 |
| `segment_id` 命名 | 命名格式为 `rank-<rank>-<medium-lower>` | 全局字段规则 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 17 `GET /query_segment?segment=...`

### 作用

查询指定 segment 的容量占用信息。

### curl

```bash
curl "http://127.0.0.1:8000/query_segment?segment=rank-0-hbm"
```

### 请求参数

| 参数名 | 类型 | 必填 | 描述 |
|---|---|---|---|
| `segment` | String | 是 | 目标 `segment_id` |

### 成功示例

`application/json; charset=utf-8`：

```json
{
  "segment": "rank-0-hbm",
  "medium": "HBM",
  "total_bytes": 5368709120,
  "used_bytes": 368640,
  "remaining_bytes": 5368340480,
  "remaining_ratio": 0.9999313354
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `segment` | 查询到的 `segment_id` | 接口成功返回约定 |
| `medium`=`HBM`\|`DRAM`\|`SSD` | segment 介质类型，示例为 `HBM` 或 `DRAM` | segment 信息 |
| `total_bytes` | segment 总容量，单位 `bytes` | segment 信息 |
| `used_bytes` | 已使用容量，单位 `bytes` | segment 信息 |
| `remaining_bytes` | 剩余容量，单位 `bytes` | 由总量与已用量计算 |
| `remaining_ratio` | 剩余比例，范围 `[0,1]` | 由容量计算 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 18 `POST /api/v1/drain_jobs`

### 作用

目标契约为返回固定字段顺序的单行文本摘要；当前源码尚未按该契约实现。

### curl

```bash
curl -X POST "http://127.0.0.1:8000/api/v1/drain_jobs"
```

### 请求参数

无

### 成功示例

无；该接口当前版本不提供成功返回。

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "Not supported",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；该接口当前固定返回 `false` | “不实现，统一错误格式”要求 |
| `error_message` | 实际错误原因；该接口当前为 `Not supported` | 未实现接口约定 |
| `timestamp` | 错误返回格式中的时间戳字段 | 统一错误格式 |

## 19 `GET /api/v1/drain_jobs/query?job_id=...`

### 作用

目标契约为返回固定字段顺序的单行文本摘要；当前源码尚未按该契约实现。

### curl

```bash
curl "http://127.0.0.1:8000/api/v1/drain_jobs/query?job_id=job_1"
```

### 请求参数

| 参数名 | 类型 | 必填 | 描述 |
|---|---|---|---|
| `job_id` | String | 是 | drain job 标识 |

### 成功示例

无；该接口当前版本不提供成功返回。

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "Not supported",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；该接口当前固定返回 `false` | “不实现，统一错误格式”要求 |
| `error_message` | 实际错误原因；该接口当前为 `Not supported` | 未实现接口约定 |
| `timestamp` | 错误返回格式中的时间戳字段 | 统一错误格式 |

## 20 `POST /api/v1/drain_jobs/cancel?job_id=...`

### 作用

当前版本不实现该接口；返回统一错误格式，实际 `error_message` 为 `Not supported`。

### curl

```bash
curl -X POST "http://127.0.0.1:8000/api/v1/drain_jobs/cancel?job_id=job_1"
```

### 请求参数

| 参数名 | 类型 | 必填 | 描述 |
|---|---|---|---|
| `job_id` | String | 是 | drain job 标识 |

### 成功示例

无；该接口当前版本不提供成功返回。

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "Not supported",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；该接口当前固定返回 `false` | “不实现，统一错误格式”要求 |
| `error_message` | 实际错误原因；该接口当前为 `Not supported` | 未实现接口约定 |
| `timestamp` | 错误返回格式中的时间戳字段 | 统一错误格式 |

## 21 `GET /api/v1/segments/status?segment=...`

### 作用

查询指定 segment 的状态。当前版本仅返回 `OK`。

### curl

```bash
curl "http://127.0.0.1:8000/api/v1/segments/status?segment=rank-0-hbm"
```

### 请求参数

| 参数名 | 类型 | 必填 | 描述 |
|---|---|---|---|
| `segment` | String | 是 | 目标 `segment_id` |

### 成功示例

`application/json; charset=utf-8`：

```json
{
  "success": true,
  "segment": "rank-0-hbm",
  "status": 1,
  "status_name": "OK"
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 接口调用成功标记；成功时为 `true` | 成功体契约 |
| `segment` | 查询到的 `segment_id` | 请求参数回显 |
| `status` | 当前状态码；当前版本固定为 `1` | 当前接口行为 |
| `status_name` | 当前状态名称；当前版本固定为 `OK` | 当前接口行为 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 22 `GET /api/v1/capacity/usage`

### 作用

返回整体容量使用情况。介质映射关系为 `HBM -> npu`、`DRAM -> cpu`；无对应介质时返回 `0`，不视为错误。

### curl

```bash
curl "http://127.0.0.1:8000/api/v1/capacity/usage"
```

### 请求参数

无

### 成功示例

`application/json; charset=utf-8`：

```json
{
  "timestamp": 0,
  "data_source_ready": true,
  "degraded": false,
  "npu": {
    "total_bytes": 0,
    "used_bytes": 0,
    "free_bytes": 0,
    "usage_ratio": 0
  },
  "cpu": {
    "total_bytes": 0,
    "used_bytes": 0,
    "free_bytes": 0,
    "usage_ratio": 0
  }
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `timestamp` | 成功响应时间戳；示例中的 `0` 为占位值，错误响应中同名字段表示错误响应生成时间 | 扩展接口通用字段 |
| `data_source_ready` | 底层数据源是否已准备就绪 | 统计接口降级规则 |
| `degraded` | 当前是否为降级返回 | 统计接口降级规则 |
| `npu` | HBM 维度容量统计 | `HBM -> npu` 映射规则 |
| `cpu` | DRAM 维度容量统计 | `DRAM -> cpu` 映射规则 |
| `*_bytes` | 容量字段单位均为 `bytes` | 全局单位规则 |
| `usage_ratio` | 使用比例，范围 `[0,1]` | 全局单位规则 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 23 `GET /api/v1/capacity/segment_remaining`

### 作用

返回各 segment 的剩余容量情况。

### curl

```bash
curl "http://127.0.0.1:8000/api/v1/capacity/segment_remaining"
```

### 请求参数

无

### 成功示例

`application/json; charset=utf-8`：

```json
{
  "timestamp": 0,
  "data_source_ready": true,
  "degraded": false,
  "segments": [
    {
      "segment_name": "rank-0-hbm",
      "medium": "HBM",
      "total_bytes": 0,
      "used_bytes": 0,
      "remaining_bytes": 0,
      "remaining_ratio": 0
    }
  ]
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `timestamp` | 成功响应时间戳；示例中的 `0` 为占位值，错误响应中同名字段表示错误响应生成时间 | 扩展接口通用字段 |
| `data_source_ready` | 底层数据源是否已准备就绪 | 统计接口降级规则 |
| `degraded` | 当前是否为降级返回 | 统计接口降级规则 |
| `segments` | segment 剩余容量列表 | 接口成功返回约定 |
| `segment_name` | segment 名称，格式 `rank-<rank>-<medium-lower>` | 全局字段规则 |
| `total_bytes` / `used_bytes` / `remaining_bytes` | 容量字段单位均为 `bytes` | 全局单位规则 |
| `remaining_ratio` | 剩余比例，范围 `[0,1]` | 全局单位规则 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 24 `GET /api/v1/analysis/alloc_free_latency`

### 作用

返回 alloc/free 延迟相关的 ptracer 文本结果。该接口展示 alloc/free 相关统计行，数据来源与 `/metrics/ptracer` 保持一致。

### curl

```bash
curl "http://127.0.0.1:8000/api/v1/analysis/alloc_free_latency"
```

### 请求参数

无

### 成功示例

`text/plain; charset=utf-8`：

```text
TIME                   NAME                                    BEGIN          GOOD_END       BAD_END        ON_FLY         P50(us)        P99(us)        P999(us)       AVG(us)        MAX(us)
2026-01-05 14:57:03    TP_MMC_META_ALLOC                         8              8              0              0              31.880         110.250        131.870        59.379         131.870
2026-01-05 14:57:03    TP_MMC_META_REMOVE                         8              8              0              0              14.820         55.000         62.020         31.398         62.020
```

**解释**

| 内容 | 含义 | 来源 |
|---|---|---|
| alloc/free 相关 ptracer 行 | 返回 alloc/free 相关统计行，文本格式与 ptracer 输出保持一致 | 接口成功返回约定 |
| `TP_MMC_META_ALLOC` / `TP_MMC_META_REMOVE` | 示例中的 ptracer 打点名称 | ptracer 输出 |
| `P50(us)` / `P99(us)` / `P999(us)` / `AVG(us)` / `MAX(us)` | 延迟统计字段，单位均为 `us` | ptracer 输出 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |

## 25 `GET /kv_events/status`

### 作用

返回 KV Event Publisher 的运行状态和统计信息。

### curl

```bash
curl "http://127.0.0.1:8000/kv_events/status"
```

### 请求参数

无

### 成功示例

`application/json; charset=utf-8`：

```json
{
   "dropped_events":0,
   "dropped_high_priority_events":0,
   "dropped_stored_events":0,
   "enabled":true,
   "last_sequence":0,
   "published_batches":0,
   "published_by_medium":{
      "dram":0,
      "hbm":0,
      "ssd":0,
      "unknown":0
   },
   "published_by_type":{
      "cleared":0,
      "removed":0,
      "stored":0
   },
   "published_events":0,
   "publisher_active":true,
   "queue_capacity":65536,
   "queue_size":0,
   "skipped_unparsed_keys":0
}

```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `enabled` | KV Event 功能是否启用（配置 + 构建开关） | 运行时状态 |
| `publisher_active` | 发布线程是否正常运行 | 实时状态 |
| `published_batches` | 已发布的批次总数 | 累计统计 |
| `published_events` | 已发布的事件总数 | 累计统计 |
| `published_by_type` | 按事件类型（stored / removed / cleared）分组的发布统计 | 累计统计 |
| `published_by_medium` | 按存储介质（hbm / dram / ssd / unknown）分组的发布统计 | 累计统计 |
| `last_sequence` | 最后发布的 ZMQ 序列号 | 实时状态 |
| `dropped_events` | 因队列满丢弃的事件总数 | 累计统计 |
| `dropped_stored_events` | 丢弃的 stored 类型事件数 | 累计统计 |
| `dropped_high_priority_events` | 丢弃的高优先级事件数（removed / cleared） | 累计统计 |
| `skipped_unparsed_keys` | 无法解析为 hash 的 key 数量 | 累计统计 |
| `queue_size` | 当前队列中的事件数量 | 实时状态 |
| `queue_capacity` | 队列最大容量（配置值） | 配置参数 |

### 错误示例

`application/json; charset=utf-8`：

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**解释**

| 字段 | 含义 | 来源 |
|---|---|---|
| `success` | 请求是否成功；错误场景固定为 `false` | 全局统一错误返回规则 |
| `error_message` | 实际错误原因；此处仅为格式示例 | 全局错误返回格式规则 |
| `timestamp` | 错误返回格式中的时间戳字段 | 全局错误返回格式规则 |
