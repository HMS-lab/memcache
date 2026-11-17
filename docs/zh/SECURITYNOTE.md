# Security Notes

## Communication Matrix

| Source | Source IP | Source Port | Destination | Destination IP | Listening Port | Protocol | Port Purpose | Port Changeable | Authentication |
|---|---|---|---|---|---|---|---|---|---|
| Local/Client | Client communication IP | Random port (assigned by the OS) | MetaService | `<ip>` in `meta_service_url` | `<port>` in `meta_service_url`; default 5000, range [1025, 65535] | TCP | Metadata object management | Yes | TLS |
| Local/Client | Client communication IP | Random port (assigned by the OS) | MetaService (etcd HA mode) | Bindable leader address published by MemFabric through the config store | Dynamically assigned MetaService listening port in [9000, 65535]; narrowed with `MEMFABRIC_HYBRID_CONFIG_STORE_PORT_START/END` | TCP | Metadata object management; in etcd HA mode, `meta_service_url` is only a configuration value and MemFabric assigns the actual port | No (assigned by MemFabric) | TLS |
| MemoryFabric instance | Client communication IP | Random port (assigned by the OS) | Config store (etcd HA leader) | Leader address written to etcd by MemFabric | Dynamically assigned config store listening port in [9000, 65535]; same range as MetaService, but a different port | TCP | Exchange and synchronize BM information; in etcd HA mode, `config_store_url` points to etcd and MemFabric assigns the actual leader port | No (assigned by MemFabric) | TLS |
| MemoryFabric instance | Client communication IP | Random port (assigned by the OS) | MemoryFabric instance | `<ip>` in `config_store_url` | `<port>` in `config_store_url`; default 6000, range [1025, 65535] | TCP | Exchange and synchronize BM information in MemoryFabric | Yes | TLS |
| HCOM participant | Client communication IP | Random port (assigned by the OS) | HCOM participant | `<ip>` in `hcom_url` | `<port>` in `hcom_url`; default 7000, range [1024, 65535] | TCP/RDMA/SDMA | HCOM communication; with multiple LocalService instances, the actual port is the base port plus `local_rank` (see below) | Yes | TLS |
| Management/monitoring client | Client communication IP | Random port (assigned by the OS) | MetaService | `<ip>` in `metrics_url` | `<port>` in `metrics_url`; default 8000, range [1025, 65535] | HTTPS/HTTP | RESTful API management and monitoring | Yes | Optional mTLS (disabled by default) / none |
| ZMQ channel publisher | Client communication IP | `<port>` in `kv_events.endpoint`; no listener by default (`kv_events.enable=false`); required when enabled, e.g. 5557 | ZMQ subscriber | One-way broadcast; not applicable | One-way broadcast; not applicable | ZMTP (ZeroMQ over TCP) | Sends KV events (`kvevent`) through ZMQ. In the decoupled architecture, this channel carries KV event notifications and decouples producers from subscribers. | Yes | Optional CURVE (disabled by default) |

- TLS private keys, certificates, and passwords can be configured in configuration files to enable secure TLS connections.
- mTLS (mutual authentication) can be configured separately for the metrics HTTP service using the `ock.mmc.metrics.tls.*` options.
- Enable TLS and store private keys encrypted to protect communications.
- After startup, delete sensitive local files such as private keys and certificates when they are no longer needed.
- Set the certificate check interval and expiration warning period with the `ACCLINK_CHECK_PERIOD_HOURS` and `ACCLINK_CERT_CHECK_AHEAD_DAYS` environment variables.
- When multiple LocalService instances are used, each instance uses the configured HCOM base port plus its `local_rank`. The listening port range is [base port, base port + world_size - 1]. HCOM retries failed connections by incrementing the port by 1 (up to 8 attempts), potentially reaching base port + world_size - 1 + 7. Therefore, **base port + world_size - 1 <= 65535** is required (maximum base port = 65535 - (world_size - 1)). MemCache rejects configurations exceeding this limit during configuration loading rather than at runtime.
- In etcd HA mode, MemFabric automatically selects **two different free ports** in the default range [9000, 65535]: one for the config store (its leader address is written to etcd), and one for MetaService (the bindable leader address used by LocalService/clients for direct connections after discovery). Narrow the range with `MEMFABRIC_HYBRID_CONFIG_STORE_PORT_START` / `MEMFABRIC_HYBRID_CONFIG_STORE_PORT_END` (aliases: `MF_CONFIG_STORE_PORT_START` / `MF_CONFIG_STORE_PORT_END`). MemFabric assigns both ports; MemCache cannot specify them. A range that is too narrow or has occupied ports can cause leader election to fail.

| Environment Variable | Description |
|---|---|
| `ACCLINK_CHECK_PERIOD_HOURS` | Certificate check interval in hours. If the value is not an integer or is outside [24, 24 × 30], the default `7 × 24` is used. |
| `ACCLINK_CERT_CHECK_AHEAD_DAYS` | Certificate expiration warning period in days. If the value is not an integer, is outside [7, 180], or converts to fewer hours than the check interval, the default 30 is used. |

## Recommended Runtime User

- For security, avoid running commands as root or another administrator account. Follow the principle of least privilege.

## Recommended Maximum File Permissions

- Set the system `umask` to `0027` or more restrictive on hosts (including host machines) and in containers. This limits new directories to at most `750` and new files to at most `640`.
- Set permissions on files, data, and directories used or generated by this project according to the recommendations below.

| Type | Recommended Maximum Linux Permission |
|---|---|
| User home directory | 750 (`rwxr-x---`) |
| Program files (including scripts and libraries) | 550 (`r-xr-x---`) |
| Program directories | 550 (`r-xr-x---`) |
| Configuration files | 640 (`rw-r-----`) |
| Configuration directories | 750 (`rwxr-x---`) |
| Completed or archived log files | 440 (`r--r-----`) |
| Active log files | 640 (`rw-r-----`) |
| Log directories | 750 (`rwxr-x---`) |
| Debug files | 640 (`rw-r-----`) |
| Debug directories | 750 (`rwxr-x---`) |
| Temporary directories | 750 (`rwxr-x---`) |
| Maintenance and upgrade directories | 770 (`rwxrwx---`) |
| Business data files | 640 (`rw-r-----`) |
| Business data directories | 750 (`rwxr-x---`) |
| Directories containing key components, private keys, certificates, or ciphertext | 700 (`rwx------`) |
| Key components, private keys, certificates, or encrypted ciphertext | 600 (`rw-------`) |
| Encryption/decryption interfaces and scripts | 500 (`r-x------`) |

## `acc_links` API List

### TCP Server Module

| API Function | Declaration |
|---|---|
| Create a TCP server | `static AccTcpServerPtr Create();` |
| Start the server | `int32_t Start(const AccTcpServerOptions &opt);` |
| Start the server with TLS authentication | `int32_t Start(const AccTcpServerOptions &opt, const AccTlsOption &tlsOption);` |
| Stop the server | `void Stop();` |
| Connect to another server | `int32_t ConnectToPeerServer(const std::string &peerIp, uint16_t port, const AccConnReq &req, uint32_t maxRetryTimes, AccTcpLinkComplexPtr &newLink);` |
| Register a handler for new request events | `void RegisterNewRequestHandler(int16_t msgType, const AccNewReqHandler &h);` |
| Register a handler for broken-link events | `void RegisterLinkBrokenHandler(const AccLinkBrokenHandler &h);` |
| Register a handler for new-link events | `void RegisterNewLinkHandler(const AccNewLinkHandler &h);` |
| Register a password decryption handler | `void RegisterDecryptHandler(const AccDecryptHandler &h);` |
| Load the dynamic library required for security authentication | `int32_t LoadDynamicLib(const std::string &dynLibPath);` |

## Dependency Software

This project depends on CANN and Ascend HDK at runtime. Refer to the [CANN](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1beta1/index/index.html) and [Ascend HDK](https://support.huawei.com/enterprise/zh/undefined/ascend-hdk-pid-252764743) installation guides and select the corresponding versions.

## Public URLs in the Source Code

| Type | Open-source Repository | File | Public IP/URL/Domain/Email | Purpose |
|---|---|---|---|---|
| Third-party dependency | <https://gitcode.com/GitHub_Trending/go/googletest.git> | `.gitmodules` | <https://gitcode.com/GitHub_Trending/go/googletest.git> | Unit test framework |
| Third-party dependency | <https://github.com/sinojelly/mockcpp.git> | `.gitmodules` | <https://github.com/sinojelly/mockcpp.git> | Mock framework for unit tests |
| Third-party dependency | <https://gitcode.com/GitHub_Trending/sp/spdlog.git> | `.gitmodules` | <https://gitcode.com/GitHub_Trending/sp/spdlog.git> | Logging framework |
| Third-party dependency | <https://gitcode.com/GitHub_Trending/js/json.git> | `.gitmodules` | <https://gitcode.com/GitHub_Trending/js/json.git> | JSON parsing and serialization |
| Third-party dependency | <https://github.com/biaks/prometheus-cpp-lite.git> | `.gitmodules` | <https://github.com/biaks/prometheus-cpp-lite.git> | Prometheus metrics |
| Third-party dependency | <https://gitcode.com/gh_mirrors/ms/msgpack-c.git> | `.gitmodules` | <https://gitcode.com/gh_mirrors/ms/msgpack-c.git> | KV event serialization |
| Third-party dependency | <https://gitcode.com/gh_mirrors/li/libzmq.git> | `.gitmodules` | <https://gitcode.com/gh_mirrors/li/libzmq.git> | ZMQ transport for KV events |
| Third-party dependency | <https://gitcode.com/gh_mirrors/li/libsodium.git> | `.gitmodules` | <https://gitcode.com/gh_mirrors/li/libsodium.git> | Low-level CURVE encryption for ZMQ |
| License URL | N/A | `LICENSE` | <http://www.apache.org/licenses/> | License file |
| License URL | N/A | `LICENSE` | <http://www.apache.org/licenses/LICENSE-2.0> | License file |
| Repository URL | <https://gitcode.com/Ascend/memcache> | `setup.py` | <https://gitcode.com/Ascend/memcache> | Repository URL in wheel package metadata |
| Repository URL | <https://gitcode.com/Ascend/memfabric_hybrid> | `performance_compare.sh` | <https://gitcode.com/Ascend/memfabric_hybrid> | Utility script |
