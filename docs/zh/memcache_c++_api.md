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
- `protocol`: Data transfer protocol token, such as `host_rdma`, `host_urma`, `host_tcp`, `host_shm`, `device_rdma`, `device_urma`, or `device_sdma`. Layered protocols use colons to order layers from outer to inner and support multiple layers, for example `host_shm:device_rdma` and `host_shm:device_rdma:device_urma`.
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
virtual int Remove(const std::string &key, uint32_t flags = 0) = 0;
```

**Function**

Delete the data object with the specified key.

**parameter**

- `key`：数据键，非空且长度 `<= 256` 字节。
- `flags`：删除标志位，默认 `0`，含未知位返回 `MMC_INVALID_PARAM`：
  - `0`（默认）：立即摘除索引，底层内存与已登记 SSD 副本在后台异步释放（有活跃租约时等待释放或超时）。
  - `MMC_REMOVE_FLAG_LAZY`（`1`）：懒删除。不立即摘除索引，仅标记为优先淘汰（按首次懒删除先后顺序）。
    - 期间 `GetInto`/`IsExist`/`GetKeyInfo`/`BatchAddLease` 等访问会恢复热度并取消标记。
    - 淘汰降级到低层后按普通数据对待；SSD-only key（未置 `KEEP_SSD`）退化为默认删除。
    - `lruType=sharded_lru` 下仅保证同分片内先后顺序，跨分片及候选列表路径不保证全局 FIFO。
  - `MMC_REMOVE_FLAG_KEEP_SSD`（`2`）：仅释放内存层并保留 SSD 副本；无 SSD 副本时退化为默认删除。
  - `MMC_REMOVE_FLAG_LAZY | MMC_REMOVE_FLAG_KEEP_SSD`（`3`）：内存层懒删除，淘汰时保留 SSD 副本；仅 SSD 副本时直接返回成功。

**说明**

- **UBS IO / SSD 删除边界**：仅对 MetaService 已登记的 `MEDIA_SSD` 副本发起异步尽力删除（失败仅告警、不重试）。
  - `metadata_mode=aggregated`：已完成刷盘并回填元数据的 SSD 副本会异步删除；刷盘在途未登记时不触发 UBS IO 删除。
  - `metadata_mode=separated`：MetaService 不登记 SSD 元数据，默认删除与 `KEEP_SSD` 均只清内存层，不删 UBS IO / SSD 数据。
- **本地缓存**：返回 `0` 时清理本进程 GVA 追踪记录（含懒删除成功）。

**return value**

- `0`：成功（不含后台异步释放与 SSD 删除结果）
- 其他：负错误码（如 `MMC_INVALID_PARAM`、`MMC_UNMATCHED_KEY`）

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
virtual std::vector<int> BatchRemove(const std::vector<std::string> &keys, uint32_t flags = 0) = 0;
```

**Function**

按顺序批量删除多个键（服务端逐 key 处理，不保证整批原子性）。

**parameter**

- `keys`：数据键列表，每个键非空且长度 `<= 256` 字节；为空时直接返回空列表。
- `flags`：删除标志位，默认 `0`，含义与 `Remove` 一致；含未知位时服务端逐 key 返回 `MMC_INVALID_PARAM`：
  - `0`：立即摘除索引，底层内存与已登记 SSD 副本异步释放（有活跃租约时等待释放或超时）。
  - `MMC_REMOVE_FLAG_LAZY`（`1`）：懒删除（优先淘汰，访问取消标记；SSD-only 退化为默认删除；详见 `Remove`）。
  - `MMC_REMOVE_FLAG_KEEP_SSD`（`2`）：仅释放内存层并保留 SSD 副本；无 SSD 副本时退化为默认删除。
  - `MMC_REMOVE_FLAG_LAZY | MMC_REMOVE_FLAG_KEEP_SSD`（`3`）：内存层懒删除，淘汰时保留 SSD 副本。

**说明**

- **SSD 边界**：`aggregated` 仅异步尽力删已登记 SSD 副本（刷盘在途未登记不触发）；`separated` 仅清内存层，不删 UBS IO。
- **部分非法 key**：空串或超长（`> 256` 字节）key 本地过滤并填 `MMC_INVALID_PARAM`，不影响同批合法 key。
- **本地缓存**：返回 `0` 的 key 会清理本进程 GVA 追踪记录（含懒删除成功）。

**return value**

与 `keys` 等长的结果列表（`keys` 为空返回空列表）：

- 单个元素 `0` 表示成功，负错误码表示失败（如 `MMC_INVALID_PARAM`、`MMC_UNMATCHED_KEY`）。
- 客户端未初始化或整批 RPC 失败时，整个列表填同一负错误码（本地内存分配失败时全填 `-1`）。

#### RemoveAll

```c++
virtual int RemoveAll() = 0;
```

**功能**

清空 MetaService 当前管理的全部键值对元数据，并异步释放关联数据。

**说明**

- **不支持 `flags`**：无懒删除或保留 SSD 选项，直接清除索引内全部 key（含已标记懒删除的 key）。
- **异步释放**：返回 `0` 仅表示索引已清、本进程 GVA 追踪已清且后台任务已提交；有活跃租约时会等待释放或超时。
- **SSD 边界**：`aggregated` 仅异步尽力删已登记 SSD 副本（刷盘在途未登记不触发，失败仅告警）；`separated` 不删 UBS IO。
- **并发语义**：`lruType=sharded_lru` 下逐分片加锁清空，不保证并发写入时的全局原子快照。

**返回值**

- `0`：清空请求完成（不含后台异步释放及 SSD 删除结果；客户端在 RPC 成功后即返回 `0`）
- 其他：失败（客户端未初始化或 RPC 失败）

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
