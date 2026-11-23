# Lease Expiration During Inference

## Symptoms

The inference framework fails to retrieve KV cache during inference and reports errors such as:

```ini
batch get lease expired.
batch copy read lease expired
lease expired after read.
```

## Root Cause

Before reading data, the client requests metadata from MetaService, which records the current time and a lease period (default: 10 seconds). Reads that exceed that period report the lease-expiration errors above. A large batch copy can trigger this issue.

## Solution

Increase the MetaService setting `ock.mmc.meta.lease_ttl_ms`, for example to `50000`.
