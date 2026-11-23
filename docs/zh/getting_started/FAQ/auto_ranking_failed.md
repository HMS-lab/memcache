# Memory Pool Initialization Reports AutoRanking failed

## Symptoms

The inference framework reports the following errors during startup:

```ini
2026-07-08 16:20:08.129923 ERROR [1757-1757][SMEM smem_bm_entry_manager.cpp:152 AutoRanking] AutoRanking failed, deviceId: 0, ret: -1, dataSize: 0
2026-07-08 16:20:08.129934 ERROR [1757-1757][SMEM smem_bm_entry_manager.cpp:78 Initialize] [RECOVER] Initialize attempt 5/5 failed, ret=-1 deviceId=0
2026-07-08 16:20:08.129939 ERROR [1757-1757][SMEM smem_bm_entry_manager.cpp:86 Initialize] initialize failed: -1
2026-07-08 16:20:08.130005 ERROR [1757-1757][SMEM smem_bm.cpp:86 smem_bm_init] init bm entry manager failed, result: -1
2026-07-08 16:20:08.130018 ERROR [1757-1757][MMC mmc_bm_proxy.cpp:55

```

## Root Cause

The total rank count exceeds `ock.mmc.local_service.world_size` in `mmc_local.conf` (default: 256). The rank count is the total number of devices used plus the number of separately deployed processes.

## Solution

Set `ock.mmc.local_service.world_size` in `mmc_local.conf` to at least the total rank count.
