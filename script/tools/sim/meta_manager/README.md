# MetaManager simulator

Sequential block read simulator against the native C++ MetaManager.

## Setup

Requires Linux, Python 3.10+, C++17 compiler and Make/Ninja.

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -e .
```

## Common arguments

| Argument | Meaning | Default |
| --- | --- | --- |
| `--pools COUNT:SIZE,...`    | Allocator groups: `COUNT` pools, each with capacity `SIZE` | `1:20GiB`     |
| `--lease-ttl-ms MS`         | Lease duration in milliseconds                             | `10000`       |
| `--min-water-level PERCENT` | Low eviction watermark                                     | `80`          |
| `--max-water-level PERCENT` | High eviction watermark                                    | `90`          |
| `--eviction-policy POLICY`  | Eviction policy: `lru` or `sharded_lru`                    | `sharded_lru` |
| `--lru-shards N`            | Shard count for `sharded_lru`                              | `16`          |
| `--memory-sample-every N`   | Sample RSS every `N` reads. `0` means initial/final only   | `1`           |
| `--memory-plot PATH`        | Path for the optional memory and cached-key plots          | `None`        |
| `--detailed-stats`          | Collect additional metrics (pool, keys, sizes, timings)    | `False`       |
| `--profile`                 | Collect C++ heap allocations with Heaptrack                | `False`       |

Heaptrack requires a separate installation. Captures are saved in `build/profiles/`.

## Random simulator - `sim_random.py`

| Argument                         | Meaning                                    | Default  |
| -------------------------------- | ------------------------------------------ | -------- |
| `--seed N`                       | Seed for workload generation               | `0`      |
| `--num-reads N`                  | Number of reads to generate                | Required |
| `--num-blocks N`                 | Number of distinct blocks                  | Required |
| `--size-weights SIZE:WEIGHT,...` | Relative weights for assigning block sizes | Required |
| `--hot-block-fraction F`         | Fraction of blocks in the hot set          | `0.2`    |
| `--hot-read-probability P`       | Probability of reading a hot block         | `0.8`    |

## Trace simulator - `sim_trace.py`

| Argument                        | Meaning                            | Default  |
| ------------------------------- | ---------------------------------- | -------- |
| `--trace-path PATH`             | Input JSONL trace file             | Required |
| `--line N`                      | Trace line to read (zero-based)    | `0`      |
| `--model-block-size MODEL:SIZE` | Bytes per block from a given model | Required |

## Bundled examples

```bash
bash examples/random_example.sh
bash examples/trace_example.sh
```
