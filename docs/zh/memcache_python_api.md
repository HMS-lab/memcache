# MemCache Python API documentation

## Overview

MemCache Python API is a high-performance distributed memory cache client that provides rich interfaces for data storage, retrieval and management. The API is based on C++ core implementation, provides a simple and easy-to-use interface through Python binding, supports multiple data transmission protocols and memory management modes, and is suitable for large-scale distributed computing environments.

### Main features

- Supports multiple transmission protocols (TCP, RDMA, SDMA, etc.).
- Supports host memory (DRAM) and device memory (HBM).
- Provides a batch operation interface to improve processing efficiency.
- Supports data copies and tiered storage.
- Provides flexible configuration options.
- Support TLS encrypted communication.

### Applicable scenarios

- Data sharing in large-scale distributed training.
- Data caching in high performance computing environments.
- Application scenarios that require low-latency data access.
- Cross-process and cross-node data exchange.

## Installation guide

### Prerequisites

- Python >= 3.8 environment (supports source code compilation)
- Supported operating systems: Linux

### Installation steps

Install the memcache_hybrid package from PyPI, which contains complete Python bindings.

```bash
pip install memcache_hybrid
```

📦 Package details: <https://pypi.org/project/memcache_hybrid/>

## core interface

### Interface overview

The Python interface encapsulates the client function of MemCache. Before use, you need to configure and start an independent meta service process.

### MetaConfig class

MetaConfig is the startup configuration class of the metadata service, which is used to set the listening address, log parameters, HA switch and TLS related parameters of the MetaService.

**Function**

Create and manage metadata service startup configurations.

**Parameter description**

| Parameter name | Type | Default value | Description |
| --------- | ------ | ------- | ------ |
| meta_service_url | str | "tcp://127.0.0.1:5000" | Metadata service startup address |
| config_store_url | str | "tcp://127.0.0.1:6000" | Config store address |
| metrics_url | str | `"http://127.0.0.1:8000"` | Metrics HTTP service address |
| ha_enable | bool | false | Whether to enable high availability deployment |
| log_level | str | "info" | Log level: debug, info, warn, error |
| log_path | str | "/var/log/memcache_hybrid" | Log directory |
| log_rotation_file_size | int | 20 | Log rotation file size, unit MB |
| log_rotation_file_count | int | 50 | Number of log rotation files |
| log_output_target | str | "file" | Log output target: screen (stdout only), file (rolling file only), both (stdout + rolling file) |
| evict_threshold_high | int | 90 | evict high water mark percentage |
| evict_threshold_low | int | 80 | evict low water level percentage |
| evict_check_interval_seconds | int | 5 | Periodic eviction check interval, in seconds; when set to 0, periodic checking is disabled; in HA mode, only the elected Master performs periodic checking, Standby and nodes with unknown roles do not perform |
| net_pool_base | int | 16 | Number of threads in the MetaService asynchronous network thread pool; range [4, 256]. Takes effect after MetaService restarts. |
| ubs_io_enable | bool | false | Whether to enable UBS_IO |
| tls_enable | bool | false | Whether to enable TLS for metaservice |
| tls_ca_path | str | "" | TLS CA certificate path |
| tls_ca_crl_path | str | "" | TLS CA CRL path |
| tls_cert_path | str | "" | TLS certificate path |
| tls_key_path | str | "" | TLS private key path |
| tls_key_pass_path | str | "" | TLS private key password path |
| tls_package_path | str | "" | TLS package path |
| tls_decrypter_path | str | "" | TLS decryption library path |
| config_store_tls_enable | bool | false | Whether to enable TLS for config_store |
| config_store_tls_ca_path | str | "" | Config store TLS CA certificate path |
| config_store_tls_ca_crl_path | str | "" | Config store TLS CA CRL path |
| config_store_tls_cert_path | str | "" | Config store TLS certificate path |
| config_store_tls_key_path | str | "" | Config store TLS private key path |
| config_store_tls_key_pass_path | str | "" | Config store TLS private key password path |
| config_store_tls_package_path | str | "" | Config store TLS package path |
| config_store_tls_decrypter_path | str | "" | Config store TLS decryption library path |

**Example**

```python
from memcache_hybrid import MetaConfig

# Create a metadata service configuration object
config = MetaConfig()
config.meta_service_url = "tcp://127.0.0.1:5000"
config.config_store_url = "tcp://127.0.0.1:6000"
config.metrics_url = "http://127.0.0.1:8000"
config.ha_enable = False
config.log_level = "info"
config.log_path = "/tmp/memcache_hybrid"
config.log_rotation_file_size = 20
config.log_rotation_file_count = 10
config.log_output_target = "both"
config.evict_threshold_high = 90
config.evict_threshold_low = 80
config.evict_check_interval_seconds = 5
```

### MetaService class

MetaService is the metadata service class of MemCache, which is used to start the metadata service directly in the Python process. Usually, the configuration is constructed through MetaConfig first, then MetaService.setup(config) is called to set the startup parameters, and finally MetaService.main() is called to block the startup service.

#### setup

```python
MetaService.setup(config)
```

**Function**

Set the startup configuration used by MetaService.main() in the current process.

**parameter**

`config`: MetaConfig object, containing the startup configuration of the metadata service

**return value**

- `0`: Success
- Others: failed

**Usage Example**

```python
from memcache_hybrid import MetaConfig, MetaService

config = MetaConfig()
config.meta_service_url = "tcp://127.0.0.1:5000"
config.config_store_url = "tcp://127.0.0.1:6000"
config.metrics_url = "http://127.0.0.1:8000"
config.log_level = "info"

MetaService.setup(config)
```

#### main

```python
MetaService.main()
```

**Function**

Start the metadata service process directly. This is a blocking call and will not return until the service receives an exit signal.

**parameter**

none

**return value**

No return value. The service will always be blocked when it is running normally, and an exception will be thrown when the service exits abnormally.

**Exception handling**

- When the service fails to start, a RuntimeError exception will be thrown, including specific error information.
- A RuntimeError exception will be thrown when a fatal error is encountered while the service is running.
- A KeyboardInterrupt exception will be thrown when the service is interrupted by a signal.

**Usage Example**

```python
from memcache_hybrid import MetaConfig, MetaService

config = MetaConfig()
config.meta_service_url = "tcp://127.0.0.1:5000"
config.config_store_url = "tcp://127.0.0.1:6000"
config.metrics_url = "http://127.0.0.1:8000"
config.log_level = "info"

try:
    assert MetaService.setup(config) == 0, "setup meta config failed"
    MetaService.main()
except KeyboardInterrupt:
    print("MetaService stopped by user")
except RuntimeError as e:
    print(f"MetaService failed to start or encountered an error: {e}")
```

**Notes**

- `MetaService.main()` is a blocking call and is recommended to be run in a separate process.
- If `MetaService.setup(config)` is not called, the service attempts to read the configuration file pointed to by the environment variable `MMC_META_CONFIG_PATH`.
- After the service is stopped, all client connections that rely on the service will become invalid.

### LocalConfig class

LocalConfig is the configuration class of distributed memory cache, used to set various parameters of the client.

**Function**

Create and manage client configurations.

**Parameter description**

| Parameter name | Type | Default value | Description |
| --------- | ------ | ------- | ------ |
| meta_service_url | str | "tcp://127.0.0.1:5000" | The URL address of Meta service, in the format of "tcp://192.168.1.1:5000" |
| config_store_url | str | "tcp://127.0.0.1:6000" | Config store URL address |
| log_level | str | "info" | Log level: debug, info, warn, error |
| world_size | int | 256 | Maximum number of ranks supported |
| protocol | str | "host_rdma" | Data transmission protocol: host_rdma, host_urma, host_tcp, host_shm, device_rdma, device_urma, device_sdma |
| hcom_url | str | "tcp://127.0.0.1:7000" | HCOM URL for RDMA network |
| dram_size | str | "1GB" | DRAM space usage, supported formats such as 1GB, 2MB, etc. |
| hbm_size | str | "0" | HBM space usage |
| max_dram_size | str | "1TB" | Maximum dram_size among all local processes |
| max_hbm_size | str | "0" | Maximum value of hbm_size among all local processes |
| client_retry_milliseconds | int | 0 | Total retry time (milliseconds) when the client requests meta service |
| client_timeout_seconds | int | 60 | Client request timeout (seconds) |
| read_thread_pool_size | int | 4 | Read thread pool size |
| write_thread_pool_size | int | 4 | Write thread pool size |
| aggregate_io | bool | true | Whether to enable read/write aggregation |
| aggregate_num | int | 122 | Aggregation number |
| ubs_io_enable | bool | false | Whether to enable UBS_IO |
| tls_enable | bool | false | Whether to enable TLS for metaservice |
| tls_ca_path | str | "" | TLS CA certificate path |
| tls_ca_crl_path | str | "" | TLS CA CRL path |
| tls_cert_path | str | "" | TLS certificate path |
| tls_key_path | str | "" | TLS key path |
| tls_key_pass_path | str | "" | TLS key password path |
| tls_package_path | str | "" | TLS package path |
| tls_decrypter_path | str | "" | TLS decryption library path |
| config_store_tls_enable | bool | false | Whether to enable TLS for config_store |
| config_store_tls_ca_path | str | "" | Config store TLS CA certificate path |
| config_store_tls_ca_crl_path | str | "" | Config store TLS CA CRL path |
| config_store_tls_cert_path | str | "" | Config store TLS certificate path |
| config_store_tls_key_path | str | "" | Config store TLS key path |
| config_store_tls_key_pass_path | str | "" | Config store TLS key password path |
| config_store_tls_package_path | str | "" | Config store TLS package path |
| config_store_tls_decrypter_path | str | "" | Config store TLS decryption library path |
| hcom_tls_enable | bool | false | Whether to enable TLS for HCOM |
| hcom_tls_ca_path | str | "" | HCOM TLS CA certificate path |
| hcom_tls_ca_crl_path | str | "" | HCOM TLS CA CRL path |
| hcom_tls_cert_path | str | "" | HCOM TLS certificate path |
| hcom_tls_key_path | str | "" | HCOM TLS key path |
| hcom_tls_key_pass_path | str | "" | HCOM TLS key password path |
| hcom_tls_decrypter_path | str | "" | HCOM TLS decrypter library path |

**Example**

```python
# Create configuration object
config = LocalConfig()
config.meta_service_url = "tcp://192.168.1.1:5000"
config.protocol = "device_sdma"
config.dram_size = "2GB"
config.hbm_size = "8GB"
config.client_timeout_seconds = 30
```

### DistributedObjectStore class

DistributedObjectStore is a Python interface encapsulation class of distributed object storage, providing a complete distributed memory cache operation interface.

#### \_\_init\_\_

```python
store = DistributedObjectStore()
```

**Function**

Create a DistributedObjectStore instance.

#### setup

```python
result = store.setup(config)
```

**Function**

Set up the distributed memory cache client configuration.

**parameter**

`config`: LocalConfig object, containing all configuration parameters of the client.

**return value**

- `0`: Success
- Others: failed

#### init

```python
result = store.init(device_id, init_bm=True)
```

**Function**

Initialize the distributed memory cache client.

**parameter**

- `device_id`: NPU card user ID when using HBM (supports ASCEND_RT_VISIBLE_DEVICES mapping).
- `init_bm`: Whether to initialize BM to provide memory, the default value is True. When set to False, pure client mode will be enabled and data reading and writing operations will not be supported.

**return value**

- `0`: Success
- Others: failed

#### get_local_service_id

```python
store.get_local_service_id()
```

**Function**

Get the local service ID.

#### close

```python
store.close()
```

**Function**

Shut down and clean the distributed memory cache client.

#### put

```python
result = store.put(key, buf, replicateConfig=defaultConfig)
```

**Function**

Write the data of the specified key into the distributed memory cache.

**parameter**

- `key`: key of data, string type
- `buf`: Bytes of data to be stored
- `replicateConfig`: Replication configuration, please refer to the ReplicateConfig data structure for details.

**return value**

- `0`: Success
- Others: failed

#### put_batch

```python
def put_batch(self, keys: List[str], values: List[bytes], replicateConfig: ReplicateConfig = defaultConfig) -> int
```

**Function**

Store multiple objects in a single batch operation to increase processing efficiency.

**parameter**

- `keys` (List[str]): List of object identifiers.
- `values` (List[bytes]): List of binary data to be stored.
- `replicateConfig` (ReplicateConfig, optional): Replication configuration for all objects.

**return value**

`int`: status code (0: success; non-zero: failure error code)

**Example**

```python
keys = ["key1", "key2", "key3"]
values = [b"value1", b"value2", b"value3"]
result = store.put_batch(keys, values)
assert result == 0, f"put_batch failed: {result}"
```

#### put_from

```python
result = store.put_from(key, buffer_ptr, size, direct=memcache_hybrid.H2G, replicateConfig=defaultConfig)
```

**Function**

Write data from a pre-allocated buffer, suitable for scenarios that require efficient memory management.

**parameter**

- `key`: The key of the data.
- `buffer_ptr`: buffer pointer.
- `size`: data size.
- `direct`: Data copy direction, optional values:
    - `memcache_hybrid.H2G`: from host memory to global memory (default).
    - `memcache_hybrid.L2G`: From card memory to global memory.
- `replicateConfig`: Replication configuration, please refer to the ReplicateConfig data structure for details.

**return value**

- `0`: Success
- Others: failed

#### batch_put_from

```python
result = store.batch_put_from(keys, buffer_ptrs, sizes, direct=memcache_hybrid.H2G, replicateConfig=defaultConfig)
```

**Function**

Write data in batches from pre-allocated buffers to improve processing efficiency.

**parameter**

- `keys`: Key list.
- `buffer_ptrs`: List of buffer pointers.
- `sizes`: Data size list.
- `direct`: Data copy direction, optional values:
    - `memcache_hybrid.H2G`: from host memory to global memory (default).
    - `memcache_hybrid.L2G`: From card memory to global memory.
- `replicateConfig`: Replication configuration, please refer to the ReplicateConfig data structure for details.

**return value**

Result list, each element represents the result of the corresponding write operation:

- `0`: Success
- Other: Error

#### put_from_layers

```python
result = store.put_from_layers(key, buffer_ptrs, sizes, direct=memcache_hybrid.H2G, replicateConfig=defaultConfig)
```

**Function**

Write hierarchical data from multiple pre-allocated buffers, suitable for storage of complex data structures.

**parameter**

- `key`: The key of the data.
- `buffer_ptrs`: List of buffer pointers, each pointer corresponds to a data layer.
- `sizes`: Data size list, each element corresponds to the size of a data layer.
- `direct`: Data copy direction, optional values:
    - `memcache_hybrid.H2G`: from host memory to global memory (default).
    - `memcache_hybrid.L2G`: From card memory to global memory.
- `replicateConfig`: Replication configuration, please refer to the ReplicateConfig data structure for details.

**return value**

- `0`: Success
- Others: failed

#### batch_put_from_layers

```python
result = store.batch_put_from_layers(keys, buffer_ptrs_list, sizes_list, direct=memcache_hybrid.H2G,
                                     replicateConfig=defaultConfig)
```

**Function**

Write hierarchical data in batches from multiple pre-allocated buffers to improve processing efficiency.

**parameter**

- `keys`: List of data keys, each key corresponds to a hierarchical data object.
- `buffer_ptrs_list`: A two-dimensional list of buffer pointers. The outer list corresponds to each key, and the inner list corresponds to each data layer pointer of each key.
- `sizes_list`: A two-dimensional list of data sizes. The outer list corresponds to each key, and the inner list corresponds to the size of each data layer for each key.
- `direct`: Data copy direction, optional values:
    - `memcache_hybrid.H2G`: from host memory to global memory (default).
    - `memcache_hybrid.L2G`: From card memory to global memory.
- `replicateConfig`: Replication configuration, please refer to the ReplicateConfig data structure for details.

**return value**

Result list, each element represents the result of the corresponding write operation:

  - `0`: Success
  - Other: Error

#### get

```python
data = store.get(key)
```

**Function**

Get the data of the specified key.

**parameter**

`key`: The key of the data.

**return value**

- Returns a data byte string on success.
- Returns an empty byte string on failure.

#### get_batch

```python
def get_batch(self, keys: List[str]) -> List[bytes]
```

**Function**

Retrieve multiple objects in a single batch operation to improve processing efficiency.

**parameter**

`keys`(List[str]): List of object identifiers to retrieve.

**return value**

- `List[bytes]`: Retrieved list of binary data.
- Returns a binary data list on success and an empty binary data list on failure.

**Example**

```python
keys = ["key1", "key2", "key3"]
values = store.get_batch(keys)
for key, value in zip(keys, values):
    print(f"{key}: {len(value)} bytes")
```

#### get_into

```python
result = store.get_into(key, buffer_ptr, size, direct=memcache_hybrid.G2H)
```

**Function**

Get data directly into a pre-allocated buffer, suitable for scenarios that require efficient memory management.

**parameter**

- `key`: The key of the data.
- `buffer_ptr`: Destination buffer pointer.
- `size`: buffer size.
- `direct`: Data copy direction, optional values:
    - `memcache_hybrid.G2H`: from global memory to host memory (default).
    - `memcache_hybrid.G2L`: From global memory to on-card memory.

**return value**

- `0`: Success
- Others: failed

#### batch_get_into

```python
results = store.batch_get_into(keys, buffer_ptrs, sizes, direct=memcache_hybrid.G2H)
```

**Function**

Get data into pre-allocated buffers in batches to improve processing efficiency.

**parameter**

- `keys`: Key list.
- `buffer_ptrs`: List of buffer pointers.
- `sizes`: List of buffer sizes.
- `direct`: Data copy direction, optional values:
    - `memcache_hybrid.G2H`: from global memory to host memory (default).
    - `memcache_hybrid.G2L`: From global memory to on-card memory.

**return value**

Result list, each element represents the result of the corresponding operation:

- `0`: Success
- Other: Error

#### get_into_layers

```python
result = store.get_into_layers(key, buffer_ptrs, sizes, direct=memcache_hybrid.G2H)
```

**Function**

Obtain data hierarchically into pre-allocated buffers, suitable for reading complex data structures.

**parameter**

- `key`: The key of the data.
- `buffer_ptrs`: List of target buffer pointers, each pointer corresponds to a data layer.
- `sizes`: Buffer size list, each element corresponds to the size of a data layer.
- `direct`: Data copy direction, optional values:
    - `memcache_hybrid.G2H`: from global memory to host memory (default).
    - `memcache_hybrid.G2L`: From global memory to on-card memory.

**return value**

- `0`: Success
- Others: failed

#### batch_get_into_layers

```python
results = store.batch_get_into_layers(keys, buffer_ptrs_list, sizes_list, direct=memcache_hybrid.G2H)
```

**Function**

Get hierarchical data into pre-allocated buffers in batches to improve processing efficiency.

**parameter**

- `keys`: List of data keys, each key corresponds to a hierarchical data object.
- `buffer_ptrs_list`: A two-dimensional list of buffer pointers. The outer list corresponds to each key, and the inner list corresponds to each target data layer pointer of each key.
- `sizes_list`: A two-dimensional list of buffer sizes. The outer list corresponds to each key, and the inner list corresponds to the size of each data layer for each key.
- `direct`: Data copy direction, optional values:
    - `memcache_hybrid.G2H`: from global memory to host memory (default).
    - `memcache_hybrid.G2L`: From global memory to on-card memory.

**return value**

Result list, each element represents the result of the corresponding operation:

- `0`: Success
- Other: Error

#### remove

```python
result = store.remove(key)
```

**Function**

Delete the data of the specified key.

**parameter**

`key`: The data key to be deleted.

**return value**

- `0`: Success
- Others: failed

#### remove_batch

```python
results = store.remove_batch(keys)
```

**Function**

Delete data in batches to improve processing efficiency.

**parameter**

`keys`: List of keys to delete.

**return value**

Result list, each element represents the result of the corresponding delete operation:

- `0`: Success
- Other: Error

#### remove_all

```python
result = store.remove_all()
```

**Function**

Delete all key-value pairs in the memory pool.

**return value**

- `0`: Success
- Others: failed

#### is_exist

```python
result = store.is_exist(key)
```

**Function**

Check whether the specified key exists.

**parameter**

`key`: The key to check.

**return value**

- `1`: exists
- `0`: does not exist
- Other: Error

#### batch_is_exist

```python
results = store.batch_is_exist(keys)
```

**Function**

Check whether keys exist in batches to improve processing efficiency.

**parameter**

`keys`: List of keys to check.

**return value**

The result list, each element represents the existence status of the corresponding key:

- `1`: exists
- `0`: does not exist
- Other: Error

#### get_key_info

```python
key_info = store.get_key_info(key, flag=0)
```

**Function**

Get the data information of the specified key.

**parameter**

- `key`: key of data
- `flag`: Query flag, the default value of the Python interface is `GET_KEY_INFO_FOR_LAYER_WISE` (LayerWise mode query, recovery from SSD when there is no DRAM).

**return value**

`KeyInfo` object, including the following methods:

  - `size()`: Get the data size.
  - `loc_list()`: Get a list of data locations.
  - `type_list()`: Get a list of data types.
  - `gva_list()`: Get the GVA list corresponding to each blob.
  - `__str__()`: Get the string representation of the information.

#### batch_get_key_info

```python
key_infos = store.batch_get_key_info(keys, flag=0)
```

**Function**

Obtain data information of multiple keys in batches to improve processing efficiency.

**parameter**

- `keys`: Data key list
- `flag`: Query flag, the default value of the Python interface is `GET_KEY_INFO_FOR_LAYER_WISE` (LayerWise mode query, recovery from SSD when there is no DRAM).

**return value**

List of `KeyInfo` objects, each object contains the following methods:

  - `size()`: Get the data size.
  - `loc_list()`: Get a list of data locations.
  - `type_list()`: Get a list of data types.
  - `gva_list()`: Get the GVA list corresponding to each blob.
  - `__str__()`: Get the string representation of the information.

#### prefetch

```python
result = store.prefetch(keys, src_media=2, dst_media=1, flags=0)
```

**Function**

Rewarm specified keys from source media to target media. Currently only SSD→DRAM (`src_media=2, dst_media=1`) is supported, other combinations are not supported yet.

**parameter**

- `keys`: Key list to be warmed up.
- `src_media`: Source media type (0=HBM, 1=DRAM, 2=SSD), default `2` (SSD), reserved parameters.
- `dst_media`: Target media type, default `1` (DRAM), reserved parameters.
- `flags`: reserved flag, must be `0`.

**return value**

- `int`: `0` indicates success, other values ​​indicate failure.

**Example**

```python
result = store.prefetch(["key1", "key2"])
assert result == 0
```

#### evict

```python
result = store.evict(keys, src_media=1, dst_media=2, flags=0)
```

**Function**

Deports the specified keys from the source media to the target media. Currently only DRAM→SSD (`src_media=1, dst_media=2`) is supported, other combinations are not supported yet. When the SSD is unavailable or the DRAM usage is higher than the recovery waterline, the data on the DRAM will be deleted directly.

**parameter**

- `keys`: List of keys to be evicted.
- `src_media`: Source media type (0=HBM, 1=DRAM, 2=SSD), default `1` (DRAM), reserved parameters.
- `dst_media`: Target media type, default `2` (SSD), reserved parameters.
- `flags`: reserved flag, must be `0`.

**return value**

- `int`: `0` indicates success, other values ​​indicate failure.

**Example**

```python
result = store.evict(["key1", "key2"])
assert result == 0
```

#### GVA related interfaces and constants

#### get_key_info / batch_get_key_info additional instructions

- The actual signatures of these two interfaces are `store.get_key_info(key, flag=0)` and `store.batch_get_key_info(keys, flag=0)` respectively.
- `KeyInfo` In addition to `size()`, `loc_list()`, and `type_list()`, `gva_list()` is also provided to return the GVA list corresponding to each blob.

```python
key_info = store.get_key_info(key)
key_infos = store.batch_get_key_info(keys)
gvas = key_info.gva_list()
```

#### batch_add_lease

```python
results = store.batch_add_lease(keys, leaseTtlMs=0)
```

**Function**

Add read leases for multiple keys in batches, and record the read lease status required for subsequent GVA reads.

**parameter**

- `keys`: The key list to be added to the read lease cannot be empty.
- `leaseTtlMs`: The lease time to be increased, in milliseconds, the default is `0`. Use the meta-side configuration item when it is `0`
  `ock.mmc.meta.lease_ttl_ms`.

**return value**

`List[int]`: Each element represents the processing result of the corresponding key. `0` represents success, and other values ​​represent failure. The length of the returned list is consistent with `keys`.

**Instructions for use**

- This interface returns the error code of each key and does not return `KeyInfo`.
- This interface is a non-transactional interface; the failure of a certain key will not roll back the read leases that have been successfully added to other keys.
- When `leaseTtlMs` is `0`, the meta side uses the value of the configuration item `ock.mmc.meta.lease_ttl_ms` to increase the lease.
- The caller should only continue to perform subsequent GVA-based reading processes for keys whose return value is `0`.
- Typical usage is to first obtain GVA through `batch_get_key_info(keys, flag=0)`, and then call `batch_add_lease(keys)`
  Establish read lease status for subsequent GVA-based read processes.
- When the same process calls the same key repeatedly, the existing read lease in the current process will be reused and renewed on the meta side;
  Call `batch_remove_lease` once after finishing reading to release it.

```python
key_infos = store.batch_get_key_info(keys, flag=0)
gvas = [info.gva_list()[0] for info in key_infos]
results = store.batch_add_lease(keys)
valid_indices = [idx for idx, ret in enumerate(results) if ret == 0]
store.batch_copy(
    [gvas[idx] for idx in valid_indices],
    [buffer_ptrs[idx] for idx in valid_indices],
    [sizes[idx] for idx in valid_indices],
    direct=SMEMB_COPY_G2L,
)
store.batch_remove_lease([keys[idx] for idx in valid_indices])
```

#### batch_remove_lease

```python
ret = store.batch_remove_lease(keys)
```

**Function**

Remove the read leases of multiple keys in batches and clear the corresponding GVA read status in the current process.

**parameter**

`keys`: The key list to remove the read lease, cannot be empty.

**return value**

- `int`: `0` indicates that the local read lease check has passed and the lease removal request sending process has been triggered.
- Other values ​​indicate failure.

**Instructions for use**

After the caller completes the GVA-based read, it should call this interface to explicitly release the read lease established by `batch_add_lease`.

#### batch_alloc

```python
gvas = store.batch_alloc(keys, sizes, media=1, leaseTtlMs=0)
```

**Function**

Apply for global memory for multiple keys in batches and return the starting GVA corresponding to each key.

**parameter**

- `keys`: Key list to apply for memory.
- `sizes`: The data size list corresponding to each key, the length must be consistent with `keys`.
- `media`: The requested media type, the default value is `1` (`MEDIA_DRAM`); `0` represents `MEDIA_HBM`.
- `leaseTtlMs`: The lease time to be increased, in milliseconds, the default is `0`. When it is `0`, use the meta-side configuration item `ock.mmc.meta.lease_ttl_ms`.

**return value**

- `List[int]`: Each element is the starting GVA corresponding to a key.
- On a readable SSD hit with no available memory GVA, the corresponding element is `2**64 - 1` (`UINT64_MAX`). The caller skips writing that key.
- When allocation fails or the parameters are invalid, the corresponding element is `0`.
- The caller must filter out both sentinel values before converting to a NumPy integer array, copying data, or sending write-completion notifications.

#### batch_copy

```python
result = store.batch_copy(gva_ptrs, buffer_ptrs, sizes, direct=SMEMB_COPY_G2L, flag=0)
```

**Function**

Performs data copies between GVA addresses and local buffers in batches.

> [!NOTE] Description
> The write direction (`SMEMB_COPY_L2G`/`SMEMB_COPY_H2G`) only copies data and does not flip the blob status corresponding to GVA to READABLE; the caller must call `batch_write_finish` again to explicitly notify the write completion.

**parameter**

- `gva_ptrs`: GVA address list.
- `buffer_ptrs`: A list of local buffer pointers, which must correspond to `gva_ptrs` one-to-one.
- `sizes`: The size list of each copy, the length must be consistent with `gva_ptrs`.
- `direct`: Data copy direction, common values ​​include:
    - `SMEMB_COPY_L2G` (`0`): Local HBM -> Global Memory.
    - `SMEMB_COPY_G2L` (`1`): Global Memory -> Local HBM.
    - `SMEMB_COPY_G2H` (`2`): Global Memory -> Local Host DRAM.
    - `SMEMB_COPY_H2G` (`3`): Local Host DRAM -> Global Memory.
- `flag`: copy flag, default 0. The read direction setting `GET_DATA_FOR_LAYER_WISE` can skip the GVA tracker validity check and directly perform DMA copy.

**return value**

- `0`: Success
- Others: failed

#### batch_copy_layers

```python
result = store.batch_copy_layers(gva_ptrs, buffer_ptrs, sizes, direct=SMEMB_COPY_G2L, flag=0)
```

**Function**

Performs batch copies of hierarchical data between GVA addresses and local buffers.

> [!NOTE] Description
> Consistent with `batch_copy`, the writing direction only copies data and does not flip the blob state; `batch_write_finish` needs to be called after writing.

**parameter**:

- `gva_ptrs`: List of starting GVAs for each object.
- `buffer_ptrs`: Two-dimensional buffer pointer list, the outer layer corresponds to the object, and the inner layer corresponds to the buffers of each layer of the object.
- `sizes`: Two-dimensional size list, the size of each layer will be accumulated sequentially starting from the corresponding starting GVA.
- `direct`: Data copy direction, the meaning is consistent with `batch_copy`.
- `flag`: copy flag, the meaning is the same as `batch_copy`.

The outer lengths of `gva_ptrs`, `buffer_ptrs` and `sizes` must be equal, and the lengths of `buffer_ptrs[i]` and `sizes[i]` of each object must be equal; if they do not match, a failure is returned and no data copy is performed.

**return value**

- `0`: Success
- Others: failed

#### batch_write_finish

```python
results = store.batch_write_finish(keys, res)
```

**Function**

Explicitly notify the meta service that writing for the given key is complete.

> [!NOTE] Description
>
> - The caller must call this interface after the `batch_copy` / `batch_copy_layers` write direction is completed, so that the meta service will flip the corresponding blob from `ALLOCATED` to `READABLE`. After that, other processes can read the data through the `batch_get_key_info` / `batch_copy` read direction.
> - For a key that is already at `READABLE`, calling it again is idempotent and returns success directly.
> - Returns `MMC_UNMATCHED_KEY` for unallocated keys.

**parameter**

- `keys`: The completed key list has been written.
- `res`: The writing result of each key is the same length as `keys`. `0` means success (corresponding to `MMC_WRITE_OK`, the blob is translated to `READABLE`), non-`0` means failure (corresponding to `MMC_WRITE_FAIL`, the meta service will remove the blob).

**return value**

- `List[int]`: The same length as `keys`. Each element is the actual update result of the meta service for the key.
- `0` indicates success.

> [!NOTE] Typical GVA cross-process reading process:
>
> - Writing process: `batch_alloc` -> `batch_copy` / `batch_copy_layers` (writing data) -> `batch_write_finish` explicitly notifies the writing completion, and the blob is converted to READABLE.
> - Read process: `batch_get_key_info(...)` acquires GVA -> `batch_add_lease(...)` adds read lease and prepares GVA read status -> `batch_copy` / `batch_copy_layers` (read) -> `batch_remove_lease(...)` explicitly releases read lease.

#### register_buffer

```python
result = store.register_buffer(buffer_ptr, size)
```

**Function**

Register a memory buffer for accelerated operation of Device RDMA.

**parameter**

- `buffer_ptr`: buffer pointer
- `size`: buffer size

**return value**:

- `0`: Success
- Others: failed

#### unregister_buffer

```python
result = store.unregister_buffer(buffer_ptr, size)
```

**Function**

Unregister the memory buffer for accelerated operation of Device RDMA.

**parameter**

- `buffer_ptr`: buffer pointer
- `size`: buffer size

**return value**

- `0`: Success
- Others: failed

## Usage example

### Basic usage examples (recommended)

The following example shows how to configure and use DistributedObjectStore using LocalConfig:

```python
from memcache_hybrid import LocalConfig, DistributedObjectStore, ReplicateConfig

# Create configuration object
config = LocalConfig()
config.meta_service_url = "tcp://192.168.1.1:5000"
config.config_store_url = "tcp://192.168.1.1:6000"
config.protocol = "device_sdma"
config.dram_size = "10GB"
config.hbm_size = "5GB"
config.client_timeout_seconds = 30

# Initialize storage client
store = DistributedObjectStore()
result = store.setup(config)  # Set configuration
if result != 0:
    raise Exception(f"Failed to setup store: {result}")

result = store.init(device_id=0, init_bm=True)
if result != 0:
    raise Exception(f"Failed to init store: {result}")

# Perform data operations
# Create a replication configuration
replica_cfg = ReplicateConfig()
replica_cfg.replicaNum = 3
replica_cfg.preferredLocalServiceIDs = [101, 102, 103]

# Store data
buffer = bytearray(b"example_data")
result = store.put("key1", buffer, replica_cfg)
if result != 0:
    raise Exception(f"Failed to put data: {result}")

# Retrieve data
data = store.get("key1")
print(f"Retrieved data: {data}")

# Check if the data exists
exists = store.is_exist("key1")
print(f"Data exists: {exists}")

# Delete data
result = store.remove("key1")
if result != 0:
    raise Exception(f"Failed to remove data: {result}")

# close connection
store.close()
```

### Configure using environment variables

The following example shows how to configure the client using environment variables (compatible with legacy methods):

`export MMC_LOCAL_CONFIG_PATH=/path/to/local_config.conf`

```python
import os
from memcache_hybrid import DistributedObjectStore

# Create and initialize the client
store = DistributedObjectStore()
result = store.init(device_id=0)
if result != 0:
    raise Exception(f"Failed to init store: {result}")

# Perform data operations
buffer = bytearray(b"example_data")
result = store.put("key1", buffer)
if result != 0:
    raise Exception(f"Failed to put data: {result}")

# Retrieve data
data = store.get("key1")
print(f"Retrieved data: {data}")

# close connection
store.close()
```

### Batch operation example

The following example shows how to use batch operations to improve processing efficiency:

```python
from memcache_hybrid import LocalConfig, DistributedObjectStore

# Create configuration and client
config = LocalConfig()
config.meta_service_url = "tcp://192.168.1.1:5000"
config.protocol = "device_sdma"

store = DistributedObjectStore()
store.setup(config)
store.init(device_id=0, init_bm=True)

# Store data in batches
keys = ["key1", "key2", "key3", "key4", "key5"]
values = [b"value1", b"value2", b"value3", b"value4", b"value5"]
result = store.put_batch(keys, values)
if result != 0:
    raise Exception(f"Failed to put batch: {result}")

# Retrieve data in batches
retrieved_values = store.get_batch(keys)
for key, value in zip(keys, retrieved_values):
    print(f"{key}: {value}")

# Check whether data exists in batches
existence = store.batch_is_exist(keys)
print(f"Existence: {existence}")

# Delete data in batches
results = store.remove_batch(keys)
print(f"Remove results: {results}")

# close connection
store.close()
```

## Data structures and enumeration types

### BmCopyType enumeration type

Enumeration type used to specify the data copy direction:

- `H2G`: from host memory to global memory.
- `L2G`: From card memory to global memory.
- `G2H`: from global memory to host memory.
- `G2L`: From global memory to on-card memory.
- `AUTO`: Automatically select the copy direction according to the memory type.

### LocalConfig configuration class

Client configuration structure for distributed memory cache client settings. See the [LocalConfig class](#localconfig-class) parameter reference.

### ReplicateConfig configuration class

Client replication configuration structure, used to set data replication strategy:

- `replicaNum`: The number of copies, the maximum is 8, the default is 1.
- `preferredLocalServiceIDs`: Forcibly allocated instance ID list, the list size must be less than or equal to replicaNum.

## error code

| Error code | Description |
|-------|-----------|
| 0 | Operation successful |
| -1 | General error |
| -3000 | invalid parameter |
| -3001 | memory allocation failure |
| -3002 | object creation failed |
| -3003 | service is not started |
| -3004 | operation timeout |
| -3005 | Repeated call |
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
- Batch operations can significantly improve processing efficiency and are recommended for large-scale data operations.
- TLS configuration needs to ensure that the certificate path and permissions are correct.
- The client needs to maintain network connectivity with the meta service.
- Setting init_bm to False during initialization will start pure client mode and does not support data reading and writing operations.
