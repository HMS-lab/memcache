# Mixing MemCache and MemFabric Across Python Environments

## Symptoms

Mixing MemCache and MemFabric installations from different Python environments can prevent the inference framework from starting or cause data-access failures, with errors such as:

```ini
unpack response body failed, result: -1
```

The logged shared-library version may also differ from the installed MemCache and MemFabric versions:

```ini
hybm init successfully,  library version: xxx"
```

## Root Cause

The inference framework's Python environment does not match the MemCache and MemFabric paths exported in `LD_LIBRARY_PATH`, causing startup or access failures.

## Solution

Check that installed MemCache and MemFabric versions match the paths in `LD_LIBRARY_PATH`. Also check the inference framework and MemCache MetaService startup scripts.
If they differ, use matching versions.
