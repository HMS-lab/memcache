# Startup Fails Because Pool Sizes Differ Across Nodes

## Symptoms

Different pool sizes across nodes cause LocalService processes to fail during startup and mmap. Initialization logs include:

```ini
AddVaInfo failed: address overlap
```

## Root Causes

1. LocalService processes use different `ock.mmc.local_service.max.dram.size` or `ock.mmc.local_service.max.hbm.size` values. For example, process A sets `ock.mmc.local_service.max.dram.size` to 1 GB while B sets it to 2 GB.

2. Maximum DRAM/HBM sizes are unset or zero, while `ock.mmc.local_service.dram.size` or `ock.mmc.local_service.hbm.size` differs across processes. For example, A sets its HBM size to 1 GB while B sets it to 2 GB or leaves it unset.

## Solutions

1. Explicitly set `ock.mmc.local_service.max.dram.size` and `ock.mmc.local_service.max.hbm.size` consistently across all LocalService processes.

2. If maximum sizes are unset, use matching `ock.mmc.local_service.dram.size` and `ock.mmc.local_service.hbm.size` values across all LocalService processes.
