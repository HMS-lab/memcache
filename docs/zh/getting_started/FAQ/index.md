# Frequently Asked Questions and Solutions

> [!TIP] Troubleshooting Steps
>
> 1. Open the relevant FAQ page from the issue list below.
> 2. If your issue is not listed, continue with the [MemFabric Hybrid FAQ](https://memfabric-hybrid.readthedocs.io/zh-cn/latest/wiki/FAQ/). MemCache depends on MemFabric Hybrid for memory pooling and transport. That FAQ groups memory allocation, device opening, memory registration, and import troubleshooting by protocol. It is especially useful for errors from underlying modules such as `[HYBM]`, `[SMEM]`, and `[HCOM]`.

## Issue List

1. [HalMemCreate fails (ret: 6)](halmemcreate.md): Memory allocation fails because memory is insufficient or the kernel is incompatible.
2. [Store initialization fails](store_init_failed.md): HCOM cannot obtain a GID because `hcom_url` is configured incorrectly.
3. [Data inconsistency after cross-node HOST_RDMA transfers](host_rdma_data_consistency.md): The RDMA NIC MTU is not greater than 4096.
4. [Startup fails because pool sizes differ across nodes](pool_size_mismatch.md): Logs report `AddVaInfo failed: address overlap`.
5. [Frequent master switches in HA mode](ha_master_switch.md): Lease renewal fails, clocks differ, or an external process clears leases.
6. [Mixing MemCache and MemFabric across Python environments](python_env_mismatch.md): Versions and `LD_LIBRARY_PATH` differ, causing startup or access failures.
7. [Lease expiration during inference](lease_expired.md): A batch copy is too large to complete within the lease period.
8. [Memory pool initialization reports AutoRanking failed](auto_ranking_failed.md): The total cluster rank count exceeds `world_size`.
