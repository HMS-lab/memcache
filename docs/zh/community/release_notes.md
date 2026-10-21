# Release Notes

## Version Compatibility

### Product Version Information

| Item | Value |
| ---- | ---- |
| Product name | memcache |
| Product version | develop |
| Release type | General release |
| Maintenance period | Three months |

### Compatibility with Related Product Versions

**Hardware Compatibility**

| Product | Version |
| --- | --- |
| Server | <ul><li>Atlas 200T A2 Box16</li><li>Atlas 800I A2/A3 series</li><li>Atlas 800T A2/A3 series</li><li>Atlas 900 A3 SuperPoD</li></ul> |
| Platform | <ul><li>aarch64</li><li>x86</li></ul> |

**Software Compatibility**

| Product | Version |
|---|---|
| OS | Ubuntu 22.04 LTS+ |
| cmake | 3.20.x |
| gcc | 11.4+ |
| python | 3.11.10 |
| pybind11 | 2.10.3 |
| make | 4.3 |
| ninja | 1.10.1 |

## Version Compatibility Notes

None.

## Changes

### Key Feature Changes

- MemFabric supports OpenClaw workloads and long sequences, increasing Prefill throughput by 5x.
- MemFabric supports the A5 high-speed UB bus and UDMA-based D2D/H2H/RH2D/RD2H scenarios, achieving 200 GB/s single-D bandwidth across nodes with four planes.
- MemFabric supports A5 cards, servers, and supernodes, with capacity above 128 TB. In LLM workloads, Prefill throughput is 5x higher than with software enablement alone, and TTFT is improved by 80%. Existing A3 capabilities can be migrated smoothly to A5.
- MemCache supports KV Cache affinity scheduling.
- Hierarchical KV Cache loading during Prefill supports longer sequences.
- Memory pooling supports DSA on the decode side and offloading decode cache to local DDR.
- Memory pooling supports SSDs, providing ultra-large capacity for Agentic AI.
- Memory pooling supports QoS from the underlying network to resolve traffic contention and prioritize high-priority traffic during contention.

### API Changes

None.

### Issues Resolved

None.

### Known Issues

None.

## Upgrade Impact

### Impact During Upgrade

- **Business workloads**: Workloads are interrupted during the software upgrade.
- **Network communications**: No impact.

### Impact After Upgrade

None.

## Related Documentation

| Document | Description |
|---|---|
| [Run package installation](../install_run.md) | How to build, install, and use the run package; for C++ API users. |
| [Wheel installation](../install_whl.md) | How to install and use the wheel package; for Python API users. |
| [C API](../memcache_c_api.md) | Introduction to the C API and its API list. |
| [C++ API](../memcache_c++_api.md) | Introduction to the C++ API and its API list. |
| [Python API](../memcache_python_api.md) | Introduction to the Python API and its API list. |
| [RESTful API](../memcache_restful_api.md) | RESTful APIs exposed by the service. |
| [Security Notes](../SECURITYNOTE.md) | MemCache security configuration. |
| [Remote Development with DevContainer](../devcontainer_quickstart.md) | Set up VS Code Remote-SSH and DevContainer, then run all examples. |
| [C/C++ Coding Conventions](../c_cpp_naming.md) | C/C++ coding conventions. |
| [Configuration File](../memcache_config.md) | MetaService and LocalService configuration parameters. |
| [Running Examples](https://gitcode.com/Ascend/memcache/blob/master/example/examples.md) | Run end-to-end C++ and Python examples. |
