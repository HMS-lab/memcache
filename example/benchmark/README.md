# Benchmark

This Python benchmark helps measure put/get performance for the MemCache memory pool. It runs the following steps.

## 1. Start the Metadata Service

```shell
export MMC_META_CONFIG_PATH=${your config path}/mmc-meta.conf;
bash bench_start_meta.sh
```

## 2. Write Test Data

```shell
export MMC_LOCAL_CONFIG_PATH=${your config path}/mmc-local.conf;
bash bench_start.sh -t write -p 1 -b 16 -s 1048576 -n 100 -w 10 -d 1 -e memcache -l npu
```

## 3. Read Test Data

```shell
export MMC_LOCAL_CONFIG_PATH=${your config path}/mmc-local.conf;
bash bench_start.sh -t read -p 1 -b 16 -s 1048576 -n 100 -d 1 -e memcache -l npu
```

## Parameters

| **Parameter** | **Options** | **Description** |
|----------------|--------------------------|----------|
| -t | [read, write] | Select whether to write or read data |
| -p | [1, 32] | Process count, limited by the number of NPUs |
| -b | [1, ] | Batch size, or number of keys in a single I/O operation |
| -s | [1, ] | Block size: the value size associated with each key in bytes |
| -n | [10, ] | Call count (test iterations). Limited by total memory-pool capacity. Generates `batch_size * process_count * call_count` keys. |
| -w | [0, ] | Number of write warmup iterations. Warmup runs before timed writes and is excluded from performance results. Each iteration uses temporary keys that are deleted before timing. Defaults to 2. Read tests are warmed up before timing and do not require this parameter. |
| -d | [1, 2] | Test a contiguous tensor (1) or a non-contiguous tensor (2). The default non-contiguous tensor size is 61×128 KB + 61×16 KB; the contiguous tensor size is the block size. |
| -e | [memcache, mooncake] | Memory-pool backend. MemCache is supported by default; Mooncake requires its dependencies to be installed. |
| -l | [npu, cpu] | Whether the test tensor is on the NPU or CPU |
