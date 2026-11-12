# Metric definitions

Report schema version 3 uses the names below in JSON and CSV. Terminal tables
use short labels such as **Succeeded**, **Already exists**, and **Failed**.
These summaries use the existing operation counters, descriptors, timings, and
memory samples. Computing them adds no native calls, waits, or memory samples.

## Cache accesses and allocations

The following fields appear globally and in each exact-size and model row:

| Field | Meaning |
| --- | --- |
| `accesses` | Number of workload blocks looked up |
| `cache_hits` | Lookups that returned a validated readable block |
| `cache_misses` | Missing-key and unreadable-key lookups combined |
| `missing_key_misses` | Lookups whose key was absent |
| `unreadable_key_misses` | Successful lookups with no usable block, including pending writes |
| `allocation_attempts` | Explicit manager `Alloc` calls after misses |
| `allocation_successes` | Manager `Alloc` calls that returned success and the expected new backing allocation |
| `allocation_duplicates` | Manager `Alloc` calls that found the key already exists |
| `allocation_failures` | Ordinary final manager allocation failures |
| `read_completions` | Successful completion calls releasing this run's read leases |
| `write_completions` | Successful completion calls marking this run's writes readable |

An allocation success registers the key and its new backing allocation. It
still needs write completion before a read can use it. A completed report requires
every successful allocation to have a successful write completion, and every
cache hit to have a successful read completion. Unexpected native errors abort
the run instead of producing a success report.

A miss remains a miss when its subsequent allocation succeeds. An allocation
that finds an existing key is counted separately from both successes and
failures. It can occur when concurrent accesses observe a pending write or race
to allocate the same key. No additional successful fill is counted for it.

Native retries and temporary allocations inside a manager call do not add to
the explicit attempt count. Allocation failures describe the final manager
result; their capacity-versus-fragmentation cause remains unavailable.

## Rates and requested bytes

| Field | Numerator / denominator, multiplied by 100 |
| --- | --- |
| `cache_hit_rate_percent` | Cache hits / accesses |
| `cache_miss_rate_percent` | Cache misses / accesses |
| `allocation_success_percent` | Successful allocations / allocation attempts |
| `allocation_failure_percent` | Failed allocations / allocation attempts |
| `allocation_duplicate_percent` | Existing-key results / allocation attempts |
| `byte_hit_rate_percent` | Hit requested bytes / all access requested bytes |

A zero denominator produces JSON `null`, a blank CSV cell, and terminal `n/a`.
Allocation success, failure, and duplicate percentages share the same
denominator. A duplicate is included in attempts but excluded from failures.

Global and exact-size rows include `requested_access_bytes`, `cache_hit_bytes`,
`cache_miss_bytes`, `allocation_attempted_bytes`, `allocation_success_bytes`,
`allocation_failure_bytes`, and `allocation_duplicate_bytes`. Each is the sum
of requested block sizes for its outcome. Repeated accesses count repeatedly.
`byte_hit_rate_percent` is a global summary; each size row already has the
equivalent count-based hit rate. Model rows contain counts and rates without
tracking an additional model-by-size table.

These byte totals describe logical requests. They do not measure payload
traffic, physical bandwidth, or the bytes currently stored in the cache.

## Reserved memory and padding

| Field | Meaning |
| --- | --- |
| `cumulative_allocation_reserved_bytes` | Sum of returned physical reservations for successful allocations over the entire run |
| `cumulative_allocation_padding_bytes` | Cumulative reserved bytes minus successful allocation requested bytes |
| `cumulative_allocation_padding_percent` | Cumulative padding / cumulative reserved bytes, multiplied by 100 |

These totals include allocations that were subsequently evicted. Allocating
the same key again after eviction adds another reservation. They exclude
temporary native allocations for failed or duplicate calls. They are not
current occupancy or directly observed eviction volume.

Each existing pool sample includes `rank`, `media`, and:

| Field | Meaning |
| --- | --- |
| `capacity_bytes` | Mounted pool capacity |
| `reserved_bytes` | Current native reservation, including padding |
| `free_bytes` | Capacity minus reserved bytes |
| `padding_bytes` | Native internal fragmentation: reserved minus live requested bytes |
| `live_requested_bytes` | Current requested bytes in native backing allocations |
| `occupancy_percent` | Reserved / capacity, multiplied by 100 |
| `padding_percent` | Padding / reserved, multiplied by 100 |

Live requested bytes can include pending writes and backing awaiting cleanup.
`registered_keys` counts metadata entries, including unreadable keys.
`reclaimed_blobs` counts the existing native removal callbacks; it is not a
measurement of reclaimed bytes or an attribution to a particular eviction.
Samples retain the existing cleanup and timing semantics.

## Timing and exports

Each native API timing retains call count, total/min/max nanoseconds and mean
nanoseconds. `total_seconds` expresses the same accumulated call duration in
seconds. Terminal tables show total seconds and mean/max microseconds. These
are synchronous manager-call durations measured inside the native adapter.
Concurrent calls can overlap; their sum is not CPU time or a fraction of replay
wall time. Python conversion and native background work are outside this timer.

JSON retains the raw native status histogram and operation outcomes for
diagnostics. CSV exports are `summary.csv` (all scalar aggregate metrics),
`sizes.csv`, `models.csv`, `memory.csv`, and `api_timings.csv`.

## Execution metrics

| Field | Meaning |
| --- | --- |
| `elapsed_wall_seconds` | Replay wall time, including periodic/final sampling and cleanup waits |
| `accesses_per_second` | Completed accesses divided by replay wall time |
| `virtual_duration_ns` | Final virtual event timestamp, without subtracting the first arrival |
| `max_active_native_calls` | Peak simultaneous adapter `Get`, `Alloc`, and read/write completion calls |
| `max_active_operations` | Maximum number of read/write lease tokens held by the coordinator |
| `delayed_accesses` | Accesses starting after their recorded virtual arrival |
| `queue_delay_ns` | Sum of virtual start time minus recorded arrival time |
| `max_queue_delay_ns` | Largest such virtual delay |

Virtual queue delay reflects serialization within a source. It does not measure
host thread-pool queueing or native API latency. Replay wall time excludes input
preparation, build, startup, the initial sample, reporting, and destruction;
see [design](design.md) for the execution and sampling boundaries.
