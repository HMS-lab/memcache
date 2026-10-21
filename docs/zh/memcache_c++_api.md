# MemCache API

## C++ interface list

The C++ language interface is fully functional and based on object-oriented design, providing a unified `ObjectStore` abstract base class that encapsulates functions such as instance management, buffer registration, data operations, batch processing, and hierarchical tensors.

### 1. Instance creation and life cycle management interface

#### local_config

`local_config` is the local configuration type used by the `Setup` interface, defined in `src/memcache/include/mmc.h`.

> [!NOTE] Description
> It is recommended to first obtain the configuration object with default values ​​through `create_default_local_config()`, and then overwrite the fields as needed.

**Commonly used fields**

- `meta_service_url`: Meta service address.
- `config_store_url`: Configure storage service address.
- `log_level`: log level, such as `debug`, `info`, `warn`, `error`.
- `world_size`: Maximum number of ranks.
- `protocol`: Data transmission protocol, such as `host_rdma`, `host_urma`, `host_tcp`, `device_rdma`, `device_urma`, `device_sdma`.
- `hcom_url`: HCOM service address.
- `dram_size`/`hbm_size`: Local service DRAM/HBM capacity.
- `max_dram_size`/`max_hbm_size`: Total upper limit of DRAM/HBM that can be used by all local processes.

#### create_default_local_config

```c++
local_config create_default_local_config();
```

**Function**

Create a `local_config` object with built-in default values, allowing users to override only necessary configuration items.

**return value**

Returns the default initialized `local_config`.

#### ObjectStore::CreateObjectStore

```c++
static std::shared_ptr<ObjectStore> CreateObjectStore();
```

**Function**

Create a distributed memory cache storage instance.

**return value**

Returns a smart pointer managed by std::shared_ptr to ensure automatic release of resources. A non-empty shared_ptr indicates success.

#### Setup

```c++
virtual int Setup(const local_config &config);
```

**Function**

Initialize and verify the local configuration for subsequent use by `Init`.

**parameter**

`config`: local configuration (`local_config`).

**return value**

- `0`: Success
- Others: failed

**Recommended calling order**

```c++
auto store = ock::mmc::ObjectStore::CreateObjectStore();
local_config config = create_default_local_config();

// You can override the default configuration as needed, or just set config_path to point to the configuration file
int ret = store->Setup(config);
if (ret != 0) {
    return ret;
}

ret = store->Init(0, true);
if (ret != 0) {
    return ret;
}
```

#### Init

```c++
virtual int Init(const uint32_t deviceId, bool initBm = true) = 0;
```

**Function**

Initialize the current storage instance and bind it to the specified device.

**parameter**

- `deviceId`: Target device ID.
- `initBm`: Whether to initialize BM to provide memory, the default value is true. When set to false, pure client mode will be enabled and data reading and writing operations will not be supported.

**return value**

- `0`: Success
- Others: failed

#### TearDown

```c++
virtual int TearDown() = 0;
```

**Function**

Release all resources occupied by the current instance and disconnect from meta-services and local services.

**return value**

- `0`: Success
- Others: failed

### 2. Buffer registration interface

#### RegisterBuffer

```c++
virtual int RegisterBuffer(void *buffer, size_t size) = 0;
```

**Function**

Registers a user-allocated memory area with the system to enable RDMA or zero-copy transfers.

**parameter**

- `buffer`: memory starting address.
- `size`: Buffer size in bytes.

**return value**

- `0`: Success
- Others: failed

#### UnRegisterBuffer

```c++
virtual int UnRegisterBuffer(void *buffer, size_t size) = 0;
```

**Function**

Unregister a registered memory area.

**parameter**

- `buffer`: memory starting address.
- `size`: Buffer size in bytes.

**return value**

- `0`: Success
- Others: failed

### 3. Data operation interface

#### GetInto

```c++
virtual int GetInto(const std::string &key, void *buffer, size_t size, const int32_t direct = 2) = 0;
```

**Function**

Reads data for the specified key into a preallocated buffer.

**parameter**

- `key`: Data key (length < 256 bytes).
- `buffer`: Target memory address.
- `size`: buffer capacity.
- `direct`: Data flow direction.

**return value**

- `0`: Success
- Others: failed

#### PutFrom

```c++
virtual int PutFrom(const std::string &key, void *buffer, size_t size, const int32_t direct = 3,
                        const ReplicateConfig &replicateConfig = {}) = 0;
```

**Function**

Write the data in the buffer to the cache and associate it with the key.

**parameter**

- `key`: Data key (length < 256 bytes).
- `buffer`: Target memory address.
- `size`: buffer capacity.
- `direct`: Data flow direction.
- `replicateConfig`: Replica policy configuration.

**return value**

- `0`: Success
- Others: failed

#### Remove

```c++
virtual int Remove(const std::string &key) = 0;
```

**Function**

Delete the data object with the specified key.

**parameter**

`key`: Data key (length < 256 bytes).

**return value**

- `0`: Success
- Others: failed

#### IsExist

```c++
virtual int IsExist(const std::string &key) = 0;
```

**Function**

Check if the key exists.

**parameter**

`key`: Data key (length < 256 bytes).

**return value**

- `1`: exists
- `0`: does not exist
- Others: failed

#### GetKeyInfo

```c++
virtual KeyInfo GetKeyInfo(const std::string &key, uint32_t flag = 0) = 0;
```

**Function**

Get the meta information of the key.

**parameter**

- `key`: The key of the data, the length is less than 256 bytes.
- `flag`: Query flag, the default value is `0`.

**return value**

Returns KeyInfo, containing:

- `size_`: Number of data bytes.
- `blobNum_`: Number of data copies.
- `loc_`: List of locations of data copies.
- `type_`: List of media types where the data copies are located.
- `gva_`: GVA list corresponding to the data copy.

> [!NOTE] Additional notes
>
> - The actual signature of `GetKeyInfo` takes the `flag` parameter. The default value is `0`.
> - In addition to `size_`, `blobNum_`, `loc_`, and `type_`, `KeyInfo` also contains `gva_`, which is used to describe the GVA list corresponding to each blob.

### 4. Batch operation interface

#### BatchGetInto

```c++
virtual std::vector<int> BatchGetInto(const std::vector<std::string> &keys, const std::vector<void *> &buffers,
                                          const std::vector<size_t> &sizes, const int32_t direct = 2) = 0;
```

**Function**

Read multiple keys in batches into their respective buffers.

**parameter**

- `keys`: List of data keys (each key length < 256 bytes).
- `buffers`: Target memory address list, which must correspond to keys one-to-one.
- `sizes`: Buffer capacity list, must be consistent with the buffers length.
- `direct`: Data flow direction.

**return value**

Returns a list of processing results corresponding to each key. Each element `0` represents success, and a negative number represents failure.

#### BatchPutFrom

```c++
virtual std::vector<int> BatchPutFrom(const std::vector<std::string> &keys, const std::vector<void *> &buffers,
                                          const std::vector<size_t> &sizes, const int32_t direct = 3,
                                          const ReplicateConfig &replicateConfig = {}) = 0;
```

**Function**

Write multiple keys in batches.

**parameter**

- `keys`: List of data keys (each key length < 256 bytes).
- `buffers`: Target memory address list, which must correspond to keys one-to-one.
- `sizes`: Buffer capacity list, must be consistent with the buffers length.
- `direct`: Data flow direction.
- `replicateConfig`: Replica policy configuration.

**return value**

Returns a list of processing results corresponding to each key. Each element `0` represents success, and a negative number represents failure.

#### BatchRemove

```c++
virtual std::vector<int> BatchRemove(const std::vector<std::string> &keys) = 0;
```

**Function**

Delete in bulk.

**parameter**

`keys`: List of data keys (each key length < 256 bytes).

**return value**

Returns a list of processing results corresponding to each key. Each element `0` represents success, and a negative number represents failure.

#### BatchIsExist

```c++
virtual std::vector<int> BatchIsExist(const std::vector<std::string> &keys) = 0;
```

**Function**

Batch existence check.

**parameter**

`keys`: List of data keys (each key length < 256 bytes).

**return value**

- `1`: exists
- `0`: does not exist
- Others: failed

#### BatchGetKeyInfo

```c++
virtual std::vector<KeyInfo> BatchGetKeyInfo(const std::vector<std::string> &keys, uint32_t flag = 0) = 0;
```

**Function**

Query meta information in batches.

**parameter**

- `keys`: List of data keys (each key length < 256 bytes).
- `flag`: Query flag, the default value is `0`.

**return value**

Returns a list of KeyInfo, each KeyInfo contains:

- `size_`: Number of data bytes.
- `blobNum_`: Number of data copies.
- `loc_`: List of locations of data copies.
- `type_`: List of media types where the data copies are located.
- `gva_`: GVA list corresponding to the data copy.

#### PrefetchKeys

```c++
virtual int PrefetchKeys(std::vector<std::string> keys, uint8_t srcMedia = MMC_MEDIA_SSD,
                         uint8_t dstMedia = MMC_MEDIA_DRAM, uint32_t flags = 0) = 0;
```

**Function**

Rewarm specified keys from source media to target media. Currently only SSD→DRAM (`MMC_MEDIA_SSD` → `MMC_MEDIA_DRAM`) is supported. Other src/dst combinations are not supported yet and `MMC_INVALID_PARAM` will be returned.

**parameter**

- `keys`: List of data keys (each key length < 256 bytes).
- `srcMedia`: Source media type (`MMC_MEDIA_HBM`=0/`MMC_MEDIA_DRAM`=1/`MMC_MEDIA_SSD`=2), default `MMC_MEDIA_SSD`, reserved parameters.
- `dstMedia`: Target media type, default `MMC_MEDIA_DRAM`, reserved parameters.
- `flags`: reserved flag, must be `0`.

**return value**

- `0`: Success
- Others: failed

#### EvictKeys

```c++
virtual int EvictKeys(std::vector<std::string> keys, uint8_t srcMedia = MMC_MEDIA_DRAM,
                      uint8_t dstMedia = MMC_MEDIA_SSD, uint32_t flags = 0) = 0;
```

**Function**

Deports the specified keys from the source media to the target media. Currently only DRAM → SSD (`MMC_MEDIA_DRAM` → `MMC_MEDIA_SSD`) is supported. Other src/dst combinations are not supported yet and `MMC_INVALID_PARAM` will be returned. When the SSD is unavailable or the DRAM usage is above the recovery waterline, the blobs on the DRAM will be deleted directly.

**parameter**

- `keys`: List of data keys (each key length < 256 bytes).
- `srcMedia`: Source media type (`MMC_MEDIA_HBM`=0/`MMC_MEDIA_DRAM`=1/`MMC_MEDIA_SSD`=2), default `MMC_MEDIA_DRAM`, reserved parameters.
- `dstMedia`: Target media type, default `MMC_MEDIA_SSD`, reserved parameters.
- `flags`: reserved flag, must be `0`.

**return value**

- `0`: Success
- Others: failed

#### BatchAddLease

```c++
virtual std::vector<int> BatchAddLease(const std::vector<std::string> &keys, uint64_t leaseTtlMs = 0) = 0;
```

**Function**

Add read leases for multiple keys in batches, and record the read lease status required for subsequent GVA reads.

**parameter**

- `keys`: The key list to add a read lease (each key length < 256 bytes), cannot be empty.
- `leaseTtlMs`: The lease time to be increased, in milliseconds, the default is `0`. When it is `0`, use the meta-side configuration item `ock.mmc.meta.lease_ttl_ms`.

**return value**

`std::vector<int>`: Each element represents the processing result of the corresponding key. `0` represents success, and other values ​​represent failure. The length of the returned list is consistent with `keys`.

**Instructions for use**

- This interface returns the error code of each key and does not return `KeyInfo`.
- This interface is a non-transactional interface; the failure of a certain key will not roll back the read leases that have been successfully added to other keys.
- When `leaseTtlMs` is `0`, the meta side uses the value of the configuration item `ock.mmc.meta.lease_ttl_ms` to increase the lease.
- The caller should only continue to perform subsequent GVA-based reading processes for keys whose return value is `0`.
- Typical usage is to first obtain GVA through `BatchGetKeyInfo(keys)`, and then call `BatchAddLease(keys)` to establish a read lease status for subsequent GVA-based reading processes.
- When the same process calls the same key repeatedly, the existing read lease in the current process will be reused and renewed on the meta side; after completing the reading, call `BatchRemoveLease` once to release it.

#### BatchRemoveLease

```c++
virtual int BatchRemoveLease(const std::vector<std::string> &keys) = 0;
```

**Function**

Remove the read leases of multiple keys in batches and clear the corresponding GVA read status in the current process.

**parameter**

`keys`: The key list to remove the read lease (each key length < 256 bytes), cannot be empty.

**return value**

- `int`: `0` indicates that the local read lease check has passed and the lease removal request sending process has been triggered.
- Other values ​​indicate failure.

**Instructions for use**

After the caller completes the GVA-based read, it should call this interface to explicitly release the read lease established by `BatchAddLease`.

#### BatchMalloc

```c++
virtual std::vector<uintptr_t> BatchMalloc(const std::vector<std::string> &keys, const std::vector<size_t> &sizes,
                                               uint16_t media, uint64_t leaseTtlMs = 0) = 0;
```

**Function**

Apply for global memory for multiple keys in batches and return the starting GVA corresponding to each key.

**parameter**

- `keys`: The key list to apply for memory, the length must be consistent with `sizes`.
- `sizes`: List of data sizes corresponding to each key.
- `media`: The requested media type, such as `MEDIA_HBM` or `MEDIA_DRAM`.
- `leaseTtlMs`: The lease time to be increased, in milliseconds, the default is `0`. When it is `0`, use the meta-side configuration item `ock.mmc.meta.lease_ttl_ms`.

**return value**

- `std::vector<uintptr_t>`: Each element is the starting GVA corresponding to a key.
- On a readable SSD hit with no available memory GVA, the corresponding element is `UINT64_MAX`. The caller skips writing that key.
- When allocation fails or the parameters are invalid, the corresponding element is `0`.
- The caller must filter out both sentinel values and perform copies and write-completion notifications only for valid GVAs.

#### BatchCopy

```c++
virtual int BatchCopy(std::vector<void *> &gvas, std::vector<void *> &buffers, std::vector<size_t> &sizes,
                          const int32_t direct = 3, uint32_t flag = 0) = 0;
```

**Function**

Performs data copies between GVA addresses and local buffers in batches.

> [!NOTE] Description
> The write direction (such as `SMEMB_COPY_L2G`/`SMEMB_COPY_H2G`) only copies data and will not flip the blob status corresponding to GVA to READABLE; the caller must call `MmcacheStore::BatchWriteFinish` again to explicitly notify the write completion before the blob enters the readable state.

**parameter**

- `gvas`: GVA address list.
- `buffers`: local buffer list, must correspond to `gvas` one-to-one.
- `sizes`: The size list of each copy, the length must be consistent with `gvas`.
- `direct`: Data copy direction, see `smem_bm_copy_type` for the value. Common values ​​include `SMEMB_COPY_L2G`, `SMEMB_COPY_G2L`, `SMEMB_COPY_G2H`, and `SMEMB_COPY_H2G`.
- `flag`: copy flag, default 0. The read direction setting `GET_DATA_FOR_LAYER_WISE` can skip the GVA tracker validity check and directly perform DMA copy.

**return value**

- `0`: Success
- Others: failed

#### BatchWriteFinish

```c++
virtual std::vector<int> BatchWriteFinish(const std::vector<std::string> &keys,
                                         const std::vector<int32_t> &writeResults) = 0;
```

**Function**

Explicitly notify the meta service that writing for the given key is complete.

> [!NOTE] Description
> The caller must call this interface after the `BatchCopy` write direction is completed, so that the meta service will flip the corresponding blob from `ALLOCATED` to `READABLE`. After that, other processes can read the data through the `BatchGetKeyInfo` / `BatchCopy` read direction. For a key that is already at `READABLE`, calling it again is idempotent and directly returns success; for an unallocated key, `MMC_UNMATCHED_KEY` is returned.

**parameter**

- `keys`: Completed key list written, each key less than 256 bytes long.
- `writeResults`: The write result of each key is the same length as `keys`. `0` indicates success (corresponding to `MMC_WRITE_OK`, the blob is translated to `READABLE`), and non-`0` indicates failure (corresponding to `MMC_WRITE_FAIL`, the meta service will remove the blob).

**return value**

`std::vector<int>`: The same length as `keys`. Each element is the actual update result of the key by meta service; `0` indicates success.

> [!NOTE] Typical GVA cross-process reading process:
>
> - Writing process: `BatchMalloc -> BatchCopy` (writing data) -> `BatchWriteFinish` explicitly notifies the writing completion, and the blob is converted to READABLE.
> - Read process: `BatchGetKeyInfo(keys, 0)` acquires GVA -> `BatchAddLease(...)` adds read lease and prepares GVA read status -> `BatchCopy` (read) -> `BatchRemoveLease(...)` explicitly releases read lease.

### 5. Hierarchical tensor operations

#### PutFromLayers

```c++
virtual int PutFromLayers(const std::string &key, const std::vector<void *> &buffers,
                              const std::vector<size_t> &sizes, const int32_t direct = 3,
                              const ReplicateConfig &replicateConfig = {}) = 0;

```

**Function**

Concatenate multiple memory blocks (layers) and write them into the cache as a logical object and associate them with the specified key.

**parameter**

- `key`: Data key (length < 256 bytes).
- `buffers`: Multi-level memory address list.
- `sizes`: List of buffer capacity sizes for each layer, which must be consistent with the buffers length.
- `direct`: Data flow direction.
- `replicateConfig`: Replica policy configuration.

**return value**

- `0`: Success
- Others: failed

#### GetIntoLayers

```c++
virtual int GetIntoLayers(const std::string &key, const std::vector<void *> &buffers,
                              const std::vector<size_t> &sizes, const int32_t direct = 2) = 0;
```

**Function**

Reads a logical object with the specified key from the cache and distributes it to multiple destination buffers in a predefined size.

**parameter**

- `key`: Data key (length < 256 bytes).
- `buffers`: Multi-level memory address list.
- `sizes`: List of buffer capacity sizes for each layer, which must be consistent with the buffers length.
- `direct`: Data flow direction.

**return value**

- `0`: Success
- Others: failed

#### BatchPutFromLayers

```c++
virtual std::vector<int> BatchPutFromLayers(const std::vector<std::string> &keys,
                                                const std::vector<std::vector<void *>> &buffers,
                                                const std::vector<std::vector<size_t>> &sizes, const int32_t direct = 3,
                                                const ReplicateConfig &replicateConfig = {}) = 0;
```

**Function**

Batch writes multiple logical objects (each consisting of multiple layers of memory blocks) to the cache.

**parameter**

- `keys`: List of data keys (each key length < 256 bytes).
- `buffers`: A multi-layer memory address list, which must correspond to keys one-to-one.
- `sizes`: List of buffer capacity sizes for each layer, which must be consistent with the buffers length.
- `direct`: Data flow direction.
- `replicateConfig`: Replica policy configuration.

**return value**

Returns a list of processing results corresponding to each key. Each element `0` represents success, and a negative number represents failure.

#### BatchGetIntoLayers

```c++
virtual std::vector<int> BatchGetIntoLayers(const std::vector<std::string> &keys,
                                                const std::vector<std::vector<void *>> &buffers,
                                                const std::vector<std::vector<size_t>> &sizes,
                                                const int32_t direct = 2) = 0;
```

**Function**

Multiple logical objects are read from the cache in batches and distributed to their respective multi-layer buffers.

**parameter**

- `keys`: List of data keys (each key length < 256 bytes).
- `buffers`: A multi-layer memory address list, which must correspond to keys one-to-one.
- `sizes`: List of buffer capacity sizes for each layer, which must be consistent with the buffers length.
- `direct`: Data flow direction.

**return value**

Returns a list of processing results corresponding to each key. Each element `0` represents success, and a negative number represents failure.

### 6. Auxiliary interface

#### GetLocalServiceId

```c++
virtual int GetLocalServiceId(uint32_t &localServiceId) = 0;
```

**Function**

Get the local service ID associated with the current instance (for debugging or log tracing).

**parameter**

`localServiceId`: Output parameters.

**return value**

- `0`: Success
- Others: failed

## data structure

### ReplicateConfig

Replica policy configuration, including the following fields:

- `replicaNum`: Number of copies, default 1, maximum 8.
- `preferredLocalServiceIDs`: A list of local service IDs allocated with priority. The list size must be less than or equal to replicaNum.

### KeyInfo

Key element information structure, including the following fields:

- `size_`: Number of data bytes.
- `blobNum_`: Number of data copies.
- `loc_`: List of locations of data copies.
- `type_`: List of media types where the data copies are located.
- `gva_`: List of global virtual addresses for data copies.

## smem_bm_copy_type enumeration type

| Type | Value | Description |
| ------------------- | ---- | -------------------- |
| SMEMB_COPY_L2G | 0 | Copy from card memory to global memory |
| SMEMB_COPY_G2L | 1 | Copy from global memory to on-card memory |
| SMEMB_COPY_G2H | 2 | Copy from global memory to host memory |
| SMEMB_COPY_H2G | 3 | Copy from host memory to global memory |
| SMEMB_COPY_L2GH | 4 | Copy from card memory to global host memory |
| SMEMB_COPY_GH2L | 5 | Copy from global host memory to on-card memory |
| SMEMB_COPY_GH2H | 6 | Copy from global host memory to host memory |
| SMEMB_COPY_H2GH | 7 | Copy from host memory to global host memory |
| SMEMB_COPY_G2G | 8 | Copy from global memory to global memory |
| SMEMB_COPY_AUTO | 9 | Automatically select the data copy direction |

## error code

| Value | Description |
|-------|-----------|
| 0 | Operation successful |
| -1 | General error |
| -3000 | invalid parameter |
| -3001 | memory allocation failure |
| -3002 | object creation failed |
| -3003 | service is not started |
| -3004 | operation timeout |
| -3005 | calls | repeatedly
| -3006 | object already exists |
| -3007 | object does not exist |
| -3008 | not initialized |
| -3009 | Duplicate network serial number |
| -3010 | Network serial number not found |
| -3011 | has notified |
| -3012 | internal error |
| -3013 | exceeds capacity limit |
| -3014 | connection not found |
| -3015 | Network request handle not found |
| -3016 | Insufficient memory |
| -3017 | is not connected to the metadata service |
| -3018 | is not connected to the local service |
| -3019 | client is not initialized |
| -3101 | status does not match |
| -3102 | key does not match |
| -3103 | return value does not match |
| -3104 | The lease has not expired |
| -3105 | Metadata backup failed |

## Things to note

- All keys must be less than 256 bytes in length.
- Supports both synchronous and asynchronous operation modes.
- Batch operations can improve processing efficiency.
