# MemCache Hybrid RESTful API Documentation

This document defines the target contract of the MemCache Hybrid REST interface; if an interface is not currently implemented, the status will be noted in the corresponding entry.

- The default HTTP address comes from the configuration item `ock.mmc.meta_service.metrics_url`
- Default value: `http://127.0.0.1:8000`; host supports any IP or domain name
- Support independently enabling mTLS through the `ock.mmc.metrics.tls.*` configuration item
- The `Content-Type` of successful response is independently agreed by each interface.
- Business errors also return `HTTP 200`; the client needs to determine whether it is successful through the `success` field in the response body.
- Error returns uniformly use JSON format; `error_message` will change according to specific error scenarios.
- `error_message` returns the actual error cause; in scenarios such as when the interface is not implemented, `Not supported` can be returned
- If the successful response contains `timestamp`, its meaning is defined by the corresponding interface; `timestamp` in the error response indicates the error response generation time, and `0` in the example is only a placeholder value

## 1 `GET /metadata?key=...`

### Purpose

Read the specified metadata value as is; upon success, the original content is returned directly without additional JSON packaging.

### curl

```bash
curl "http://127.0.0.1:8000/metadata?key=demo_key"
```

### Request parameters

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `key` | String | Yes | The metadata key to be read |

### Successful Examples

`text/plain; charset=utf-8`:

```text
demo metadata value
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| returns the value as it is | directly returns the metadata value on success without reorganizing the outer JSON | interface contract |
| Success `Content-Type` | The example in this article uses `text/plain; charset=utf-8` | Example convention |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 2 `PUT /metadata?key=...`

### Purpose

Writes the specified metadata value as raw text.

### curl

```bash
curl -X PUT "http://127.0.0.1:8000/metadata?key=demo_key" \
  --data 'demo metadata value'
```

### Request parameters

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `key` | String | Yes | The metadata key to be written |

### Request body

The original text corresponding to `key`; the server writes the received body as it is without schema verification.

It is recommended to use `Content-Type: text/plain; charset=utf-8` for the request body.

### Successful Examples

`text/plain; charset=utf-8`:

```text
metadata updated
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| `metadata updated` | metadata fixed text after successful writing | interface successful return convention |
| Write the | request body as it is without schema verification and save it as the original text | Interface contract |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 3 `DELETE /metadata?key=...`

### Purpose

Delete the specified metadata key.

### curl

```bash
curl -X DELETE "http://127.0.0.1:8000/metadata?key=demo_key"
```

### Request parameters

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `key` | String | Yes | The metadata key to be deleted |

### Successful Examples

`text/plain; charset=utf-8`:

```text
metadata deleted
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| `metadata deleted` | metadata fixed text after successful deletion | interface successful return convention |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 4 `GET /health`

### Purpose

Returns HTTP service health status, HA status, and service readiness status.

### curl

```bash
curl "http://127.0.0.1:8000/health"
```

### Request parameters

none

### Successful Examples

`application/json; charset=utf-8`:

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

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `status` | Health status; fixed to `ok` when successful | The interface successfully returns the convention |
| `role` | Current role, value `leader / standby / unknown` | HA status snapshot |
| `ha_state` | Current HA status, value `starting / standby / serving / unknown` | HA status snapshot |
| `service_ready` | Whether the HTTP service is ready | Service running status |
| `leader_address` | leader address; fixed when there is no stable source `unknown` | HA status snapshot |
| `view_version` | View version; fixed when no stable source `0` | HA state snapshot |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 5 `GET /metrics`

### Purpose

Export MemCache monitoring metrics in Prometheus text format. Fields that are currently unavailable allow export with placeholder values ​​such as `0` or `false`.

### curl

```bash
curl "http://127.0.0.1:8000/metrics"
```

### Request parameters

none

### Successful Examples

`text/plain; version=0.0.4`:

```text
# HELP memcache_segment_capacity_bytes Segment total capacity in bytes
# TYPE memcache_segment_capacity_bytes gauge
memcache_segment_capacity_bytes{segment="rank-0-hbm"} 5368709120
# HELP memcache_segment_allocated_bytes Segment allocated bytes
# TYPE memcache_segment_allocated_bytes gauge
memcache_segment_allocated_bytes{segment="rank-0-hbm"} 368640
# HELP memcache_segment_fragmentation_bytes Segment internal fragmentation bytes (chunked minus requested)
# TYPE memcache_segment_fragmentation_bytes gauge
memcache_segment_fragmentation_bytes{segment="rank-0-hbm"} 4096
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
# HELP memcache_bandwidth_latency_seconds Client bandwidth operation latency in seconds
# HELP memcache_bandwidth_latency_seconds Client bandwidth operation latency in seconds
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

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| Prometheus text | Returns Prometheus exposition text | Successfully returns the convention |
| Successful example | Successful example uses one row of representative values ​​for each indicator to cover most indicators and label fields, not a complete example line by line; for label values, see the indicator description table below | Monitoring indicator contract |
| output sequence | indicators are output in groups. The main grouping sequence is: capacity → business call → eviction/warming/hit → `stored_keys` → KV event → client indicator | Monitoring indicator contract |
| Result semantics | `successes_total` indicates `MMC_OK`; `failures_total` indicates real errors (including `MMC_DUPLICATED_OBJECT`); `not_found_total` indicates `MMC_UNMATCHED_KEY` and is excluded from failures. Batch metrics count API calls, and batch sub-operations also contribute to the corresponding non-batch metrics. For a batch call, real errors take precedence; increment Batch `not_found_total` only when there is no real error and a sub-operation returns `MMC_UNMATCHED_KEY`. | Monitoring metric contract |
| placeholder value strategy | Indicators that currently cannot provide real values ​​are still retained in the success body, and `0` or `false` placeholders can be used | downgrade rule |

**Indicator Description**

The following table explains each item in order of output.

| indicator | type | label | data source | meaning |
|---|---|---|---|---|
| `memcache_segment_capacity_bytes` | gauge | `segment` | `RestSegmentSnapshot::totalBytes` | segment total capacity |
| `memcache_segment_allocated_bytes` | gauge | `segment` | `RestSegmentSnapshot::usedBytes` | segment allocated capacity |
| `memcache_segment_fragmentation_bytes` | gauge | `segment` | `RestSegmentSnapshot::fragmentationBytes` | segment internal fragmentation (total block size minus the sum of active blob logical request bytes, including alignment and max mode amplification) |
| `memcache_total_capacity_bytes` | gauge | `medium`=`hbm`\|`dram`\|`ssd` | `RestUsageSnapshot::totalBytes` | Total capacity grouped by medium. `ssd` is included only when SSD capacity or usage is non-zero. |
| `memcache_allocated_bytes` | gauge | `medium`=`hbm`\|`dram`\|`ssd` | `RestUsageSnapshot::usedBytes` | summarizes allocated capacity by medium; `ssd` same as |
| `memcache_<op>_requests_total` | counter | — | `MmcMetaMetricSnapshot::<op>RequestCount` | `<op>` Total number of interface calls; `<op>` ∈ {alloc, batch_alloc, get, batch_get, remove, batch_remove, remove_all, update_state, batch_update_state, query, batch_query, get_all_keys, exist_key, batch_exist_key, mount, unmount}; supports per-rank (when `IsPerRankEnabled()` is turned on, the `{rank}` label details with the same name will also be output) |
| `memcache_<op>_successes_total` | counter | — | `MmcMetaMetricSnapshot::<op>SuccessCount` | `<op>` Number of successes (`MMC_OK`); support per-rank |
| `memcache_<op>_failures_total` | counter | — | `MmcMetaMetricSnapshot::<op>FailureCount` | `<op>` Number of failures (real errors, including `MMC_DUPLICATED_OBJECT`); support per-rank |
| `memcache_<op>_not_found_total` | counter | — | `MmcMetaMetricSnapshot::<op>NotFoundCount` | `<op>` Number of misses (`MMC_UNMATCHED_KEY`), not included in failure; only `get`/`batch_get`/`remove`/`batch_remove`/`update_state`/`batch_update_state`/`query`/`batch_query`/`exist_key`/`batch_exist_key` output; supports per-rank |
| `memcache_evict_operations_total` | counter | — | `MmcMetaMetricSnapshot::evictCount` | Total number of eviction operations; supports per-rank |
| `memcache_evict_running` | gauge | — | `MmcMetaManager::evictCheck_` | Whether the elimination (GC) is being performed (1=yes, 0=no) |
| `memcache_flush_blobs_total` | counter | — | `MmcMetaMetricSnapshot::asyncFlushBlobAdded` | Asynchronous flush The total number of blobs written to SSD; supports per-rank |
| `memcache_flush_bytes_total` | counter | — | `MmcMetaMetricSnapshot::asyncFlushBlobBytes` | Asynchronous flush The total number of bytes written to SSD; supports per-rank |
| `memcache_get_hits_dram_total` | counter | — | `MmcMetaMetricSnapshot::getHitDramCount` | Get The number of keys returned by directly hitting the readable block (HBM/DRAM) without triggering warm-up; supports per-rank |
| `memcache_get_hits_ssd_total` | counter | — | `MmcMetaMetricSnapshot::getHitSsdCount` | Get The number of keys returned by the temperature recovery target block after hitting the SSD and the recovery is successful; recovery failures are not counted (counted into `rewarm_failed_total`); support per-rank |
| `memcache_rewarm_total` | counter | — | `MmcMetaMetricSnapshot::rewarmCount` | Total number of SSD→DRAM rewarming operations; support per-rank |
| `memcache_rewarm_failed_total` | counter | — | `MmcMetaMetricSnapshot::rewarmFailCount` | Number of failed warm-up operations; supports per-rank |
| `memcache_rewarm_bytes_total` | counter | — | `MmcMetaMetricSnapshot::rewarmBytesCount` | Cumulative number of recovery bytes; supports per-rank |
| `memcache_rewarm_bytes_current` | gauge | — | `MmcMetaMetricSnapshot::rewarmBytesCurrent` | Current number of recovery bytes in transit; supports per-rank |
| `memcache_stored_keys` | gauge | — | `MmcMetaMetricSnapshot::keyCount` | Currently stored key number |
| `memcache_kv_events_published_total` | counter | — | `KvEventStats::publishedEvents` | Total number of published KV events |
| `memcache_kv_events_published_by_type_total` | counter | `type`=`stored`\|`removed`\|`cleared` | `KvEventStats::publishedStoredEvents` etc. | Number of publications grouped by event type |
| `memcache_kv_events_published_by_medium_total` | counter | `medium`=`hbm`\|`dram`\|`ssd`\|`unknown` | `KvEventStats::publishedHbmEvents` etc. | Number of releases grouped by storage medium |
| `memcache_kv_events_dropped_total` | counter | — | `KvEventStats::droppedEvents` | Total number of queue full discard events |
| `memcache_kv_events_dropped_stored_total` | counter | — | `KvEventStats::droppedStoredEvents` | Number of discarded stored type events |
| `memcache_kv_events_dropped_high_priority_total` | counter | — | `KvEventStats::droppedHighPriorityEvents` | Number of discarded high priority (removed/cleared) events |
| `memcache_kv_events_skipped_unparsed_total` | counter | — | `KvEventStats::skippedUnparsedKeys` | The number of keys that cannot be parsed into hash |
| `memcache_kv_events_queue_size` | gauge | — | `KvEventStats::queueSize` | Current number of queue events |
| `memcache_kv_events_queue_capacity` | gauge | — | `KvEventStats::queueCapacity` | Queue soft capacity upper limit (configured value) |
| `memcache_kv_events_publisher_active` | gauge | — | `KvEventStats::publisherActive` | Is the publishing thread running (1/0) |
| `memcache_kv_events_last_sequence` | gauge | — | `KvEventStats::lastSequence` | Last released ZMQ serial number |
| `memcache_metric_stale` | gauge | `rank` | `RankMetricView::stale` | Whether the client metric is expired (1=expired, 0=online); the client metric is only output when there is an online `RankMetricView` |
| `memcache_bandwidth_bytes` | gauge | `rank`,`operation`=`batch_put`\|`batch_get` | `BandwidthMetricData::totalBytes` | Number of bytes transmitted in the bandwidth window (sliding window value) |
| `memcache_bandwidth_duration_ms` | gauge | `rank`,`operation`=`batch_put`\|`batch_get`\|`batch_exist` | `BandwidthMetricData::totalDurationMs` | Bandwidth window duration (ms, sliding window value) |
| `memcache_bandwidth_bytes_cum_total` | counter | `rank`,`operation`=`batch_put`\|`batch_get` | `BandwidthMetricData::cumTotalBytes` | Cumulative number of bytes transferred (cumulative since startup) |
| `memcache_bandwidth_duration_ms_cum_total` | counter | `rank`,`operation`=`batch_put`\|`batch_get`\|`batch_exist` | `BandwidthMetricData::cumTotalDurationMs` | Accumulated time (ms, accumulated since startup) |
# HELP memcache_bandwidth_latency_seconds Client bandwidth operation latency in seconds
| `memcache_bandwidth_bytes_per_sec` | gauge | `rank`,`operation`=`batch_put`\|`batch_get` | `BandwidthMetricData::bytesPerSec` | Instantaneous bandwidth (B/s) |
| `memcache_ubs_io_disk_capacity_bytes` | gauge | `rank` | `UbsIoMetricData::diskCap` | SSD total capacity |
| `memcache_ubs_io_disk_used_bytes` | gauge | `rank` | `UbsIoMetricData::diskUsed` | SSD used capacity |
| `memcache_ubs_io_mem_capacity_bytes` | gauge | `rank` | `UbsIoMetricData::memCap` | Total memory capacity |
| `memcache_ubs_io_mem_used_bytes` | gauge | `rank` | `UbsIoMetricData::memUsed` | Memory used capacity |
| `memcache_ubs_io_disk_num` | gauge | `rank` | `UbsIoMetricData::diskNum` | Total number of disks |
| `memcache_ubs_io_fault_disk_num` | gauge | `rank` | `UbsIoMetricData::faultDiskNum` | Number of failed disks |
| `memcache_ubs_io_disk_status` | gauge | `rank`,`disk_path` | `UbsIoPerDiskMetric::status` | Single disk status (0=normal) |
| `memcache_ubs_io_disk_read_bandwidth_bytes_per_sec` | gauge | `rank`, `disk_path` | `UbsIoPerDiskMetric::readBandwidth` | Single disk read bandwidth |
| `memcache_ubs_io_disk_write_bandwidth_bytes_per_sec` | gauge | `rank`, `disk_path` | `UbsIoPerDiskMetric::writeBandwidth` | Single disk write bandwidth |
| `memcache_ubs_io_disk_total_bandwidth_bytes_per_sec` | gauge | `rank`,`disk_path` | `UbsIoPerDiskMetric::totalBandwidth` | Total bandwidth of single disk |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 6 `GET /metrics/summary`

### Purpose

Returns a single-line text summary in fixed field order. It belongs to the statistics summary interface. Fields that are currently unavailable allow return of placeholder values ​​according to the downgrade policy.

The return format is a single line of text, consisting of `key=value` fragments separated by spaces; the field order is fixed, no line breaks, and no JSON packaging.

### curl

```bash
curl "http://127.0.0.1:8000/metrics/summary"
```

### Request parameters

none

### Successful Examples

`text/plain; charset=utf-8`:

```text
keys=2 evict=0 rewarm=0 rewarm_fail=0 rewarm_bytes_total=0 rewarm_bytes_current=0 flush_blobs=0 flush_bytes=0 get_hit_dram=0 get_hit_ssd=0 query_hit_keys=0 hbm_used=360KiB/5.0GiB (0.01%) dram_used=0B/5.0GiB (0.00%) ssd_used=0B/0B alloc_req=68 alloc_success=68 alloc_fail=0 batch_alloc_req=0 batch_alloc_success=0 batch_alloc_fail=0 get_req=68 get_success=68 get_fail=0 get_not_found=0 batch_get_req=0 batch_get_success=0 batch_get_fail=0 batch_get_not_found=0 remove_req=0 remove_success=0 remove_fail=0 remove_not_found=0 batch_remove_req=0 batch_remove_success=0 batch_remove_fail=0 batch_remove_not_found=0 remove_all_req=0 remove_all_success=0 remove_all_fail=0 update_state_req=0 update_state_success=0 update_state_fail=0 update_state_not_found=0 batch_update_state_req=0 batch_update_state_success=0 batch_update_state_fail=0 batch_update_state_not_found=0 query_req=12 query_success=11 query_fail=0 query_not_found=1 batch_query_req=3 batch_query_success=3 batch_query_fail=0 batch_query_not_found=1 get_all_keys_req=4 get_all_keys_success=4 get_all_keys_fail=0 exist_key_req=0 exist_key_success=0 exist_key_fail=0 exist_key_not_found=0 batch_exist_key_req=0 batch_exist_key_success=0 batch_exist_key_fail=0 batch_exist_key_not_found=0 mount_req=0 mount_success=0 mount_fail=0 unmount_req=0 unmount_success=0 unmount_fail=0
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| Single line of text | Must be a single line when successful and cannot be split by line breaks | The interface successfully returns |
| format | `key=value` string separated by spaces, field order is fixed | Statistical summary interface contract |
| Successful example fields | Successful example should contain all fields of the current contract | Statistical summary interface contract |
| Fixed field order | in this order: `keys`,`evict`,`rewarm`,`rewarm_fail`,`rewarm_bytes_total`,`rewarm_bytes_current`,`flush_blobs`,`flush_bytes`,`get_hit_dram`,`get_hit_ssd`,`query_hit_keys`,`hbm_used`,`dram_used`,`ssd_used`,`alloc_req`,`alloc_success`,`alloc_fail`,`batch_alloc_req`,`batch_alloc_success`,`batch_alloc_fail`,`get_req`,`get_success`,`get_fail`,`get_not_found`,`batch_get_req`,`batch_get_success`,`batch_get_fail`,`batch_get_not_found`,`remove_req`,`remove_success`,`remove_fail`,`remove_not_found`,`batch_remove_req`,`batch_remove_success`,`batch_remove_fail`,`batch_remove_not_found`,`remove_all_req`,`remove_all_success`,`remove_all_fail`,`update_state_req`,`update_state_success`,`update_state_fail`,`update_state_not_found`,`batch_update_state_req`,`batch_update_state_success`,`batch_update_state_fail`,`batch_update_state_not_found`,`query_req`,`query_success`,`query_fail`,`query_not_found`,`batch_query_req`,`batch_query_success`,`batch_query_fail`,`batch_query_not_found`,`get_all_keys_req`,`get_all_keys_success`,`get_all_keys_fail`,`exist_key_req`,`exist_key_success`,`exist_key_fail`,`exist_key_not_found`,`batch_exist_key_req`,`batch_exist_key_success`,`batch_exist_key_fail`,`batch_exist_key_not_found`,`mount_req`,`mount_success`,`mount_fail`,`unmount_req`,`unmount_success`,`unmount_fail` | Statistical summary interface contract |
| capacity field | `hbm_used` / `dram_used` / `ssd_used` use `used/total (xx.xx%)`. Capacities use human-readable base-1024 units (`B`/`KiB`/`MiB`/`GiB`/`TiB`/`PiB`/`EiB`), with one decimal place for values below 10 and whole numbers otherwise. Utilization has two decimal places. When `total` is 0 (no SSD configured), the output is `0B/0B` without a percentage. For exact byte counts, use `memcache_total_capacity_bytes` / `memcache_allocated_bytes` from `/metrics`. | Statistics summary interface contract |
| per-rank field | When `MmcMetaMetricManager::IsPerRankEnabled()` is turned on, the per-rank field is appended in the form of `<base>_by_rank_<rank>=<val>` after the above fixed field (each rank One item), covering evictions/warming/flush/hits: `evict_by_rank`, `get_hit_dram_by_rank`, `get_hit_ssd_by_rank`, `rewarm_by_rank`, `rewarm_fail_by_rank`, `rewarm_bytes_by_rank`, `rewarm_bytes_current_by_rank`, `flush_blobs_by_rank`, `flush_bytes_by_rank`| per-rank statistical contract |
| Result semantics | `*_success` indicates `MMC_OK`; `*_fail` indicates a real error (including `MMC_DUPLICATED_OBJECT`); `*_not_found` indicates `MMC_UNMATCHED_KEY` and is excluded from failures. Batch fields count API calls, while batch sub-operations also contribute to the corresponding non-batch fields. For a batch call, real errors take precedence; increment Batch `*_not_found` only when there is no real error and a sub-operation returns `MMC_UNMATCHED_KEY`. | Statistical summary interface contract |
| placeholder value strategy | Fields that currently cannot provide real values ​​are still retained in the success body, and `0`, `0B/0B` or null value placeholders can be used | Downgrade rule |

**Field description**

The following table explains field by field in output order.

| field | format | data source | meaning |
|---|---|---|---|
| `keys` | uint | `MmcMetaMetricSnapshot::keyCount` | Currently stored key number |
| `evict` | uint | `MmcMetaMetricSnapshot::evictCount` | Total number of eviction operations |
| `rewarm` | uint | `MmcMetaMetricSnapshot::rewarmCount` | Total number of SSD→DRAM rewarming operations |
| `rewarm_fail` | uint | `MmcMetaMetricSnapshot::rewarmFailCount` | Number of failed warm-up operations |
| `rewarm_bytes_total` | uint (bytes) | `MmcMetaMetricSnapshot::rewarmBytesCount` | Cumulative number of temperature recovery bytes |
| `rewarm_bytes_current` | uint (bytes) | `MmcMetaMetricSnapshot::rewarmBytesCurrent` | The number of bytes currently in transit |
| `flush_blobs` | uint | `MmcMetaMetricSnapshot::asyncFlushBlobAdded` | Asynchronous flush Total number of blobs written to SSD |
| `flush_bytes` | uint (bytes) | `MmcMetaMetricSnapshot::asyncFlushBlobBytes` | Asynchronous flush Total number of bytes written to SSD |
| `get_hit_dram` | uint | `MmcMetaMetricSnapshot::getHitDramCount` | Get The number of keys returned by directly hitting the readable block (HBM/DRAM) without triggering recovery |
| `get_hit_ssd` | uint | `MmcMetaMetricSnapshot::getHitSsdCount` | Get The number of keys returned by the rewarming target block after the SSD is hit and the rewarming is successful; the rewarming failure is not counted (counted into `rewarm_fail`) |
| `query_hit_keys` | uint | `MmcMetaMetricSnapshot::queryHitKeysCount` | Number of keys for which Query/BatchQuery successfully returns a valid query result |
| `hbm_used` | `used/total (xx.xx%)` (human-readable, e.g. `360KiB/5.0GiB (0.01%)`) | `RestUsageSnapshot`(hbm) | HBM used/total capacity and utilization |
| `dram_used` | `used/total (xx.xx%)` (human-readable, e.g. `26GiB/4.1TiB (0.62%)`) | `RestUsageSnapshot`(dram) | DRAM used/total capacity and utilization |
| `ssd_used` | `used/total (xx.xx%)` (human-readable, e.g. `0B/0B`) | `RestUsageSnapshot`(ssd) | SSD used/total capacity and utilization; `0B/0B` without a percentage when no SSD is configured |
| `<op>_req` | uint | `MmcMetaMetricSnapshot::<op>RequestCount` | `<op>` Total number of interface calls |
| `<op>_success` | uint | `MmcMetaMetricSnapshot::<op>SuccessCount` | `<op>` Number of successes (`MMC_OK`) |
| `<op>_fail` | uint | `MmcMetaMetricSnapshot::<op>FailureCount` | `<op>` Number of failures (real errors, including `MMC_DUPLICATED_OBJECT`) |
| `<op>_not_found` | uint | `MmcMetaMetricSnapshot::<op>NotFoundCount` | `<op>` Number of misses (`MMC_UNMATCHED_KEY`), not included in fail |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 7 `GET /metrics/ptracer`

### Purpose

Export the current ptracer raw text output.

### curl

```bash
curl "http://127.0.0.1:8000/metrics/ptracer"
```

### Request parameters

none

### Successful Examples

`text/plain; charset=utf-8`:

```text
TIME                   NAME                                    BEGIN          GOOD_END       BAD_END        ON_FLY         P50(us)        P99(us)        P999(us)       AVG(us)        MAX(us)
2026-01-05 14:57:03    TP_MMC_META_PUT                         8              8              0              0              31.880         110.250        131.870        59.379         131.870
2026-01-05 14:57:03    TP_MMC_META_GET                         8              8              0              0              14.820         55.000         62.020         31.398         62.020
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| ptracer text | Keep the current ptracer text output format when successful | The interface returns successfully |
| alloc/free dot | If alloc/free dot is added, it will also be exposed through this interface | ptracer output range |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 8 `GET /role`

### Purpose

Returns the current character text.

### curl

```bash
curl "http://127.0.0.1:8000/role"
```

### Request parameters

none

### Successful Examples

`text/plain; charset=utf-8`:

```text
leader
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| `leader` / `standby` / `unknown` | Current role text | HA status mapping result |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 9 `GET /ha_status`

### Purpose

Returns the current HA status text. Returns `unknown` when mapping cannot be stabilized.

### curl

```bash
curl "http://127.0.0.1:8000/ha_status"
```

### Request parameters

none

### Successful Examples

`text/plain; charset=utf-8`:

```text
serving
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| `starting` / `standby` / `serving` / `unknown` | Current HA status text | HA status snapshot |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 10 `GET /leader`

### Purpose

Returns whether the leader exists, its address and view version. The `role` field is not included in the response; fields that cannot currently provide stable values ​​are allowed to return default values.

### curl

```bash
curl "http://127.0.0.1:8000/leader"
```

### Request parameters

none

### Successful Examples

`application/json; charset=utf-8`:

```json
{
  "present": true,
  "leader_address": "unknown",
  "view_version": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `present` | Whether the existence of the leader can be confirmed currently | HA status snapshot |
| `leader_address` | leader address; fixed when there is no stable source `unknown` | HA status snapshot |
| `view_version` | View version; fixed when no stable source `0` | HA state snapshot |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 11 `GET /query_key?key=...`

### Purpose

Query the metadata information of a single key, including object size, access attributes, and blob distribution information.

### curl

```bash
curl "http://127.0.0.1:8000/query_key?key=key_a"
```

### Request parameters

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `key` | String | Yes | The key name to be queried |

### Successful Examples

`application/json; charset=utf-8`:

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

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `key` | Current query key | HTTP interface return field |
| `size` | Total object size | `MemObjQueryInfo::size_` |
| `prot` | Object access property | `MemObjQueryInfo::prot_` |
| `numBlobs` | Number of blobs corresponding to the current object | `MemObjQueryInfo::numBlobs_` |
| `valid` | Is the current key valid? | `MemObjQueryInfo::valid_` |
| `blobs` | blob description information list | `MemObjQueryInfo::blobs_` |
| `blobs[].rank` | blob is in rank | `MmcMemBlobDesc::rank_` |
| `blobs[].medium` | The media type string where the blob is located | `MmcMemBlobDesc::mediaType_` |
| `blobs[].gva` | Global virtual address of the blob | `MmcMemBlobDesc::gva_` |
| `blobs[].state` | The status of the blob on the meta side | `MmcMemBlobDesc::state_` |
| `blobs[].leaseTimeoutTtlMs` | Remaining read lease TTL, in milliseconds | `MmcMemBlobDesc::leaseTimeoutTtlMs_` |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 12 `GET /batch_query_keys?keys=...`

### Purpose

Query the metadata information of multiple keys in batches; the field definition of a single key is consistent with `/query_key`.

### curl

```bash
curl "http://127.0.0.1:8000/batch_query_keys?keys=key_a,key_b"
```

### Request parameters

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `keys` | String | Yes | comma separated key list |

### Successful Examples

`application/json; charset=utf-8`:

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

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the interface level call is successful; in the successful example, it is `true` | Success body contract |
| `data` | Query result array returned in request order | Success body contract |
| `data[].key` | The key | corresponding to the current result is consistent with `/query_key` |
| `data[].size` | The total size of the object | is consistent with `/query_key` |
| `data[].prot` | Object access properties | are consistent with `/query_key` |
| `data[].numBlobs` | The number of blobs corresponding to the current object | is consistent with `/query_key` |
| `data[].valid` | Is the current key valid? | is consistent with `/query_key` |
| `data[].blobs` | blob description information list | is consistent with `/query_key` |
| `data[].blobs[].rank` | blob is in rank | consistent with `/query_key` |
| `data[].blobs[].medium` | The media type string where the blob is located | is consistent with `/query_key` |
| `data[].blobs[].gva` | The global virtual address of the blob | is consistent with `/query_key` |
| `data[].blobs[].state` | The status of the blob on the meta side | is consistent with `/query_key` |
| `data[].blobs[].leaseTimeoutTtlMs` | Remaining read lease TTL, in milliseconds | is consistent with `/query_key` |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the interface level request is successful; the error scenario is fixed to `false` | Global unified error return rules |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 13 `DELETE /key?key=...`

### Purpose

Delete the specified business KV Cache key and its associated data.

### curl

```bash
curl -X DELETE "http://127.0.0.1:8000/key?key=demo_key"
```

### Request parameters

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `key` | String | Yes | The business to be deleted KV Cache key |

### Successful Examples

`application/json; charset=utf-8`:

```json
{
  "success": true,
  "message": "key deleted"
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the success scenario is fixed as `true` | The interface successfully returns the convention |
| `message` | Success prompt information | Interface success return convention |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 14 `DELETE /all_keys`

### Purpose

Delete all business KV Cache keys and their associated data.

### curl

```bash
curl -X DELETE "http://127.0.0.1:8000/all_keys"
```

### Request parameters

none

### Successful Examples

`application/json; charset=utf-8`:

```json
{
  "success": true,
  "message": "all keys deleted"
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the success scenario is fixed as `true` | The interface successfully returns the convention |
| `message` | Success prompt information | Interface success return convention |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 15 `GET /get_all_keys`

### Purpose

List all object keys.

### curl

```bash
curl "http://127.0.0.1:8000/get_all_keys"
```

### Request parameters

none

### Successful Examples

`text/plain; charset=utf-8`:

```text
key_1
key_2
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| One key per line | Returns the key list; spliced ​​line by line, with a newline at the end; when the list is empty, the body can also be empty | The interface successfully returns the convention |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 16 `GET /get_all_segments`

### Purpose

List all `segment_id`. The current version returns text line by line and does not return a JSON array.

### curl

```bash
curl "http://127.0.0.1:8000/get_all_segments"
```

### Request parameters

none

### Successful Examples

`text/plain; charset=utf-8`:

```text
rank-0-hbm
rank-1-hbm
rank-0-dram
rank-1-dram
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| One per line `segment_id` | Returns line by line when successful `segment_id` text | Fixed successful body shape |
| `segment_id` naming | naming format is `rank-<rank>-<medium-lower>` | global field rule |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 17 `GET /query_segment?segment=...`

### Purpose

Query the capacity usage information of the specified segment.

### curl

```bash
curl "http://127.0.0.1:8000/query_segment?segment=rank-0-hbm"
```

### Request parameters

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `segment` | String | Yes | target `segment_id` |

### Successful Examples

`application/json; charset=utf-8`:

```json
{
  "segment": "rank-0-hbm",
  "medium": "HBM",
  "total_bytes": 5368709120,
  "used_bytes": 368640,
  "remaining_bytes": 5368340480,
  "fragmentation_bytes": 4096,
  "remaining_ratio": 0.9999313354
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `segment` | The queried `segment_id` | interface successfully returns the convention |
| `medium`=`HBM`\|`DRAM`\|`SSD` | segment media type, example is `HBM` or `DRAM` | segment information |
| `total_bytes` | segment total capacity, unit `bytes` | segment information |
| `used_bytes` | Used capacity, unit `bytes` | segment information |
| `remaining_bytes` | Remaining capacity, unit `bytes` | Calculated from total amount and used amount |
| `fragmentation_bytes` | Internal fragmentation, unit `bytes` (total block size minus the sum of active blob logical request bytes) | segment information |
| `remaining_ratio` | Remaining proportion, range `[0,1]` | Calculated from capacity |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 18 `POST /api/v1/drain_jobs`

### Purpose

The target contract is to return a single-line text summary in a fixed field order; the current source code has not yet been implemented according to this contract.

### curl

```bash
curl -X POST "http://127.0.0.1:8000/api/v1/drain_jobs"
```

### Request parameters

none

### Successful Examples

None; the current version of this interface does not provide successful return.

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "Not supported",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the interface currently returns `false` | "Not implemented, unified error format" requirement |
| `error_message` | Actual error reason; the interface is currently `Not supported` | The interface convention is not implemented |
| `timestamp` | Timestamp field in error return format | Unified error format |

## 19 `GET /api/v1/drain_jobs/query?job_id=...`

### Purpose

The target contract is to return a single-line text summary in a fixed field order; the current source code has not yet been implemented according to this contract.

### curl

```bash
curl "http://127.0.0.1:8000/api/v1/drain_jobs/query?job_id=job_1"
```

### Request parameters

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `job_id` | String | Yes | drain job identification |

### Successful Examples

None; the current version of this interface does not provide successful return.

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "Not supported",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the interface currently returns `false` | "Not implemented, unified error format" requirement |
| `error_message` | Actual error reason; the interface is currently `Not supported` | The interface convention is not implemented |
| `timestamp` | Timestamp field in error return format | Unified error format |

## 20 `POST /api/v1/drain_jobs/cancel?job_id=...`

### Purpose

The current version does not implement this interface; it returns a unified error format, and the actual `error_message` is `Not supported`.

### curl

```bash
curl -X POST "http://127.0.0.1:8000/api/v1/drain_jobs/cancel?job_id=job_1"
```

### Request parameters

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `job_id` | String | Yes | drain job identification |

### Successful Examples

None; the current version of this interface does not provide successful return.

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "Not supported",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the interface currently returns `false` | "Not implemented, unified error format" requirement |
| `error_message` | Actual error reason; the interface is currently `Not supported` | The interface convention is not implemented |
| `timestamp` | Timestamp field in error return format | Unified error format |

## 21 `GET /api/v1/segments/status?segment=...`

### Purpose

Query the status of the specified segment. The current version only returns `OK`.

### curl

```bash
curl "http://127.0.0.1:8000/api/v1/segments/status?segment=rank-0-hbm"
```

### Request parameters

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `segment` | String | Yes | target `segment_id` |

### Successful Examples

`application/json; charset=utf-8`:

```json
{
  "success": true,
  "segment": "rank-0-hbm",
  "status": 1,
  "status_name": "OK"
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Interface call success mark; when successful, it is `true` | Success body contract |
| `segment` | Queryed `segment_id` | Request parameter echo |
| `status` | Current status code; the current version is fixed to `1` | Current interface behavior |
| `status_name` | Current state name; current version is fixed to `OK` | Current interface behavior |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 22 `GET /api/v1/capacity/usage`

### Purpose

Returns overall capacity usage. The media mapping relationship is `HBM -> npu`, `DRAM -> cpu`; if there is no corresponding media, `0` is returned and is not considered an error.

### curl

```bash
curl "http://127.0.0.1:8000/api/v1/capacity/usage"
```

### Request parameters

none

### Successful Examples

`application/json; charset=utf-8`:

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

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `timestamp` | Successful response timestamp; `0` in the example is a placeholder value, and the field with the same name in the error response indicates the error response generation time | Extended interface common field |
| `data_source_ready` | Whether the underlying data source is ready | Statistics interface downgrade rules |
| `degraded` | Return whether it is currently downgraded | Statistical interface downgrade rule |
| `npu` | HBM dimension capacity statistics | `HBM -> npu` Mapping rules |
| `cpu` | DRAM dimension capacity statistics | `DRAM -> cpu` Mapping rules |
| `*_bytes` | Capacity field units are all `bytes` | Global unit rule |
| `usage_ratio` | Use scale, range `[0,1]` | Global unit rules |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 23 `GET /api/v1/capacity/segment_remaining`

### Purpose

Returns the remaining capacity of each segment.

### curl

```bash
curl "http://127.0.0.1:8000/api/v1/capacity/segment_remaining"
```

### Request parameters

none

### Successful Examples

`application/json; charset=utf-8`:

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
      "fragmentation_bytes": 0,
      "remaining_ratio": 0
    }
  ]
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `timestamp` | Successful response timestamp; `0` in the example is a placeholder value, and the field with the same name in the error response indicates the error response generation time | Extended interface common field |
| `data_source_ready` | Whether the underlying data source is ready | Statistics interface downgrade rules |
| `degraded` | Return whether it is currently downgraded | Statistical interface downgrade rule |
| `segments` | segment remaining capacity list | interface successful return agreement |
| `segment_name` | segment name, format `rank-<rank>-<medium-lower>` | global field rule |
| `total_bytes` / `used_bytes` / `remaining_bytes` / `fragmentation_bytes` | Capacity field units are `bytes` | Global unit rule |
| `remaining_ratio` | Remaining proportion, range `[0,1]` | Global unit rule |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 24 `GET /api/v1/analysis/alloc_free_latency`

### Purpose

Returns ptracer text results related to alloc/free latency. This interface displays alloc/free related statistical rows, and the data source is consistent with `/metrics/ptracer`.

### curl

```bash
curl "http://127.0.0.1:8000/api/v1/analysis/alloc_free_latency"
```

### Request parameters

none

### Successful Examples

`text/plain; charset=utf-8`:

```text
TIME                   NAME                                    BEGIN          GOOD_END       BAD_END        ON_FLY         P50(us)        P99(us)        P999(us)       AVG(us)        MAX(us)
2026-01-05 14:57:03    TP_MMC_META_ALLOC                         8              8              0              0              31.880         110.250        131.870        59.379         131.870
2026-01-05 14:57:03    TP_MMC_META_REMOVE                         8              8              0              0              14.820         55.000         62.020         31.398         62.020
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| alloc/free related ptracer rows | returns alloc/free related statistical rows, the text format is consistent with ptracer output | The interface successfully returns the convention |
| `TP_MMC_META_ALLOC` / `TP_MMC_META_REMOVE` | ptracer dotted names in the example | ptracer output |
| `P50(us)` / `P99(us)` / `P999(us)` / `AVG(us)` / `MAX(us)` | Delay statistics field, the units are `us` | ptracer output |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 25 `GET /kv_events/status`

### Purpose

Returns the running status and statistics of KV Event Publisher.

### curl

```bash
curl "http://127.0.0.1:8000/kv_events/status"
```

### Request parameters

none

### Successful Examples

`application/json; charset=utf-8`:

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

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `enabled` | KV Event function is enabled (configuration + build switch) | Runtime status |
| `publisher_active` | Whether the publishing thread is running normally | Real-time status |
| `published_batches` | Total number of batches released | Cumulative statistics |
| `published_events` | Total number of published events | Cumulative statistics |
| `published_by_type` | Release statistics grouped by event type (stored / removed / cleared) | Cumulative statistics |
| `published_by_medium` | Release statistics grouped by storage medium (hbm/dram/ssd/unknown) | Cumulative statistics |
| `last_sequence` | Last released ZMQ serial number | Real-time status |
| `dropped_events` | Total number of events discarded due to queue full | Cumulative statistics |
| `dropped_stored_events` | Number of discarded stored type events | Cumulative statistics |
| `dropped_high_priority_events` | Number of discarded high priority events (removed/cleared) | Cumulative statistics |
| `skipped_unparsed_keys` | Number of keys that cannot be parsed into hash | Cumulative statistics |
| `queue_size` | Number of events in the current queue | Real-time status |
| `queue_capacity` | Maximum queue capacity (configuration value) | Configuration parameters |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; here is only a format example | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 26 `POST /memory/evict`

### Purpose

Evict the specified KV cache. Determine behavior based on the `locations` field:

- **`locations` or `locations` is not specified as `null`**: Delete key globally (all levels)
- **`locations` is specified**: Move the key down one level from the current level (such as DRAM → SSD)

### curl

```bash
# Delete globally (without specifying locations)
curl -X POST "http://127.0.0.1:8000/memory/evict" \
  -H "Content-Type: application/json" \
  -d '{"chunk_hashes": ["a1b2c3d4..."]}'

# Move down one level (specified locations)
curl -X POST "http://127.0.0.1:8000/memory/evict" \
  -H "Content-Type: application/json" \
  -d '{"chunk_hashes": ["a1b2c3d4..."], "locations": ["LocalCPUBackend"]}'
```

### Request parameters

The request body is in JSON format:

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `chunk_hashes` | Array[String] | Yes | The chunk hash list to be evicted |
| `locations` | Array[String] | No | Target location list; perform global deletion if not specified or `null` |

### Successful Examples

`application/json; charset=utf-8`:

```json
{
  "status": "success",
  "num_evicted": 1
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `status` | Operation status; fixed to `success` when successful | The interface successfully returns |
| `num_evicted` | The number of chunks evicted this time | The interface successfully returns the agreement |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; such as `Invalid JSON body`, `Missing 'chunk_hashes' in request body`, etc. | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 27 `POST /memory/prefetch`

### Purpose

Prefetch the specified KV cache. Front-load data from lower tiers (such as SSD) to higher tiers (such as DRAM) to speed subsequent access.

### curl

```bash
curl -X POST "http://127.0.0.1:8000/memory/prefetch" \
  -H "Content-Type: application/json" \
  -d '{"chunk_hashes": ["a1b2c3d4...", "e5f6g7h8..."], "lookup_id": "req_001"}'
```

### Request parameters

The request body is in JSON format:

| Parameter name | Type | Required | Description |
|---|---|---|---|
| `chunk_hashes` | Array[String] | Yes | Chunk hash list to be prefetched |
| `lookup_id` | String | No | Prefetch request identifier, used for tracking; if not specified, returns an empty string |

### Successful Examples

`application/json; charset=utf-8`:

```json
{
  "status": "prefetch_started",
  "lookup_id": "req_001",
  "num_chunks": 2
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `status` | Operation status; fixed to `prefetch_started` when successful | The interface successfully returns |
| `lookup_id` | Request identifier, echo the value passed in by the client | Request parameter echo |
| `num_chunks` | The number of chunks prefetched this time | The interface successfully returns the convention |

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "actual error reason",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|---|---|---|
| `success` | Whether the request is successful; the error scenario is fixed to `false` | Global unified error return rule |
| `error_message` | Actual error cause; such as `Invalid JSON body`, `Missing 'chunk_hashes' in request body`, etc. | Global error return format rules |
| `timestamp` | Timestamp field in error return format | Global error return format rules |

## 28 `GET /livez`

### Purpose

Lightweight survival probe, designed for Kubernetes probes (exec + curl). It only determines that the process is alive and that the HTTP server can handle the request. It does not perform HA status query, lock, or serialize JSON. When the process is frozen by `SIGSTOP`, the handler cannot be executed, and the probe times out and fails → triggers a container restart.

### curl

```bash
curl "http://127.0.0.1:8000/livez"
```

### Request parameters

none

### Successful Examples

`text/plain; charset=utf-8`:

```text
ok
```

**Explanation**

| Content | Meaning | Source |
|---|---|---|
| `ok` | The body returned is fixed when the process is alive and the HTTP server processes the request normally. | The interface successfully returns the convention |
| success `Content-Type` | `text/plain; charset=utf-8` | The interface successfully returns the convention |
| HTTP status code | Fixed `200` | Interface successful return convention |

### Error example

No JSON error response. When the process is abnormal (such as `SIGSTOP`, deadlock, crash), a response cannot be returned. The probe side will show a timeout or connection failure, and the above body will not be returned.

## 29 `GET /api/v1/mf/cluster_status`

### Purpose

Returns a MemFabric cluster status view: the config store server state machine, sets of live and reconnected ranks, heartbeat freshness, and
a group manager link summary. The default response is a **summary view** (`linkSummary` counts only, without abnormal-link details).
It omits the full link matrix and has O(N) output size, supporting continuous polling by Prometheus or dashboards at 30-second intervals.
When `linkSummary.abnormalLinks > 0`, append `?unstable=true` to retrieve abnormal-link details in `unstableLinks`.

- Optional query parameter `unstable`: only the exact value `true` takes effect. Other values (`false`, misspellings, etc.) are treated as if the parameter were absent, **without an error**. Other unknown query
  parameters are also ignored.
- An empty `mf_cluster` array (`[]`) means this process has no config store. This is a normal response (`status: "ok"`), distinct from an error response.

### curl

```bash
# Default: summary view (without unstableLinks details)
curl "http://127.0.0.1:8000/api/v1/mf/cluster_status"

# Drill down: abnormal-link details (useful when abnormalLinks > 0)
curl "http://127.0.0.1:8000/api/v1/mf/cluster_status?unstable=true"
```

### Request parameters

| Parameter | Type | Required | Description |
|------------|--------|----|---------------------------------------------------------------------------------------------|
| `unstable` | string | No | Only the exact value `true` adds `unstableLinks` details and, if truncated, `unstableTruncated` to `groupManager`. Other values are equivalent to omitting the parameter. |

### Successful example

`application/json; charset=utf-8`:

```json
{
  "status": "ok",
  "mf_cluster": [
    {
      "type": "tcp",
      "model": "server",
      "connect": true,
      "server": {
        "state": "SS_NORMAL",
        "worldSize": 4,
        "listen": "127.0.0.1:6000",
        "backendAliveRanks": [
          1,
          2
        ],
        "aliveRanks": [
          0,
          1,
          2
        ],
        "reconnectedRanks": [
          1
        ],
        "lastHeartbeat": [
          {
            "rank": 0,
            "sinceLastMs": 1200
          }
        ],
        "groupManager": {
          "maxRanks": 4,
          "ranks": [
            {
              "rank": 0,
              "state": "RANK_ACTIVE"
            }
          ],
          "linkSummary": {
            "activeRanks": 2,
            "connectedLinks": 2,
            "expectedLinks": 2,
            "completionRate": 1.000,
            "abnormalLinks": 0
          }
        }
      }
    }
  ]
}
```

**Explanation**

Server-level fields:

| Field | Meaning |
|----------------------------|----------------------------------------------------------------------------------------|
| `status` | Always `ok`; see the error example below for error scenarios |
| `mf_cluster` | Array of stores; may be empty (no store, not an error) |
| `server.state` | Main server state machine: `SS_INITED` / `SS_RECOVERING` / `SS_RECOVERED` / `SS_NORMAL` / `SS_EXITED` |
| `server.worldSize` | Agreed cluster size, fixed when the first rank connects. Before it is fixed, the value is `4294967295` (`UINT32_MAX`); serialization as `-1` is deferred. |
| `server.listen` | Config store server listening address (`ip:port`) |
| `server.backendAliveRanks` | Live ranks at the backend layer, in ascending order |
| `server.aliveRanks` | Live ranks at the connection layer, in ascending order. A mismatch with `backendAliveRanks` indicates a partially failed state where the connection exists but the backend is dead. |
| `server.reconnectedRanks` | Ranks that have reconnected since startup |
| `server.lastHeartbeat[]` | Heartbeat freshness per rank: `{"rank": uint, "sinceLastMs": int64}`. The interval is in milliseconds since the last heartbeat; entries are sorted by rank in ascending order. |
| `groupManager` | `null` when this process has no group manager |

groupManager-level fields:

| Field | Meaning |
|------------------------------|--------------------------------------------------------------------------------------------------------|
| `maxRanks` | Group manager matrix capacity |
| `ranks[]` | Non-IDLE rank states: `{"rank": uint, "state": "RANK_CHECKED_IN"/"RANK_ACTIVE"}`, sorted by rank in ascending order |
| `linkSummary.activeRanks` | Number of ranks in `RANK_ACTIVE` |
| `linkSummary.connectedLinks` | Number of links in `LINK_CONNECTED`, counted as ordered pairs |
| `linkSummary.expectedLinks` | Expected link count: `activeRanks × (activeRanks − 1)`. Uses activeRanks rather than worldSize so incomplete joins do not reduce the completion rate. |
| `linkSummary.completionRate` | `connectedLinks / expectedLinks`, in `[0,1]`; returns `1.0` when `expectedLinks` is 0 |
| `linkSummary.abnormalLinks` | Number of links whose endpoint ranks are both live (in `aliveRanks`) but whose state is not `LINK_CONNECTED`. Excludes expected IDLE links with a dead peer. Values above 0 warrant drilling down with `?unstable=true`. |
| `unstableLinks[]` | Abnormal-link details, **included only with `?unstable=true`**. Absent from the default response; inclusion rules follow. |

unstableLinks inclusion rules (the union of these conditions, evaluated in a single server-side traversal of the link matrix):

- Condition 1: The link is in an intermediate state: `LINK_EXCHANGING` / `LINK_EXCHANGED` / `LINK_CONNECTING` (being established or potentially stuck; remaining in `EXCHANGED` is abnormal).
- Condition 2: The link is `LINK_IDLE` and **both endpoint ranks are live** (a missing expected connection). IDLE links with a dead endpoint are expected and excluded.
- `LINK_CONNECTED` links are excluded. Link-flapping detection belongs to the event/log layer; snapshots have no history.

Each entry contains:

| Field | Meaning |
|---------------|------------------------------------------------------------------------------|
| `src` / `dst` | Source/destination rankId |
| `state` | Current link state: `LINK_IDLE` / `LINK_EXCHANGING` / `LINK_EXCHANGED` / `LINK_CONNECTING` |
| `retry` | Retry count for the current transition |
| `elapsedMs` | Milliseconds spent in the current state; `-1` for an IDLE disconnected link with no active transition |

**Output size limit**: `unstableLinks` contains at most **256** entries (`kUnstableLinksMaxEntries`, defined in
MemFabric `smem_tcp_config_store_server.h`). Excess entries are truncated after sorting by `(retry descending, elapsedMs descending)`
(links most likely to be stuck come first). The response adds `"unstableTruncated": <number of omitted entries>` only when truncation occurs.
This limit is part of the public behavior contract; changes require a documentation update.

Example fields added to `groupManager` with `?unstable=true`:

```json
"unstableLinks": [
{"src": 5, "dst": 8, "state": "LINK_EXCHANGING", "retry": 2, "elapsedMs": 4100}
],
"unstableTruncated": 127
```

### Error example

`application/json; charset=utf-8`:

```json
{
  "success": false,
  "error_message": "internal server error",
  "timestamp": 0
}
```

**Explanation**

| Field | Meaning | Source |
|-----------------|----------------------------------------------------------------------------------------------|------------|
| `success` | Whether the request succeeded; always `false` for errors | Global error response rules |
| `error_message` | Actual error reason. Returns `internal server error` when an underlying call returns a code other than `SMEM_OK` or `SMEM_NOT_EXIST`, or JSON parsing fails. | Global error response format rules |
| `timestamp` | Error response generation time; `0` in this example is a placeholder | Global error response format rules |

Note: No config store in this process **is not an error**. The response is `status: "ok"` with `mf_cluster: []`.
