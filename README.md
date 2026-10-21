<h1 align="center" style="margin: 0;"><img src="docs/zh/source/memcache_icon.png" style="width: 37%" /></h1>
<div align="center">

  <hr style="display:block; border:none; height:0; border-top:2px solid #008000; width:100%; max-width:1250px; margin:20px auto;">
  <h2 align="center">
High-performance distributed key-value cache
  </h2>

[![Docs](https://img.shields.io/badge/docs-live-brightgreen)](https://ascend-memcache.readthedocs.io/zh-cn/latest/)
[![PyPI](https://img.shields.io/pypi/v/memcache-hybrid)](https://pypi.org/project/memcache-hybrid/#history)
[![PyPI - Python Version](https://img.shields.io/pypi/pyversions/memcache-hybrid)](https://pypi.org/project/memcache-hybrid/)
[![PyPI - Downloads](https://img.shields.io/pypi/dm/memcache-hybrid)](https://pypi.org/project/memcache-hybrid/)
[![GitCode commit activity](https://img.shields.io/badge/commit_activity-high-light)](https://gitcode.com/Ascend/memcache/analysis)
[![license](https://img.shields.io/badge/license-MulanPSL2-orange)](./LICENSE)

</div>
<br/>

## 🔄 Latest News

- [2026/06] MemCache enables PrefixCache acceleration for inference. See the [wiki case library](https://gitcode.com/Ascend/memcache/wiki/Home.md) for the latest examples.
- [2025/12] MemCache is available as a vllm-ascend backend to accelerate LLM inference. See the vllm-ascend open-source community and the [usage example](https://docs.vllm.ai/projects/ascend/en/v0.27.1rc1/user_guide/feature_guide/kv_pool.html#example-of-using-memcache-as-a-kv-pool-backend).

- [2025/11] MemCache was open-sourced in November 2025. The community repository is <https://gitcode.com/Ascend/memcache>.

## 🔜 Roadmap and Release Strategy

See the MemCache roadmap: [**Roadmap**](https://gitcode.com/Ascend/memcache/wiki/Roadmap.md).
See the MemCache [**branch release strategy**](https://gitcode.com/Ascend/memcache/wiki/%E5%BC%80%E5%8F%91%E4%B8%8E%E5%8F%91%E5%B8%83%E8%8A%82%E5%A5%8F%E5%8E%9F%E5%88%99.md).

## 🎉 Overview

MemCache is a high-performance distributed KVCache storage engine designed for LLM and GR inference. Its main features include:

- **Object-based API**: Supports batched and non-batched put/get/exist/remove operations, as well as read/write APIs for multi-layer KV blocks.
- **Tiered cache pool**: Supports HBM, DDR, and SSD tiers, moving data between tiers through eviction and prefetch.
- **High bandwidth and low latency**: Uses [MemFabric](https://gitcode.com/Ascend/memfabric_hybrid) as the foundation for tiered memory and heterogeneous network transfers. On Ascend hardware, paths including device_rdma (A2), device_sdma (A3), host_rdma (A2/A3), and device_urma (A5) provide OneCopy direct data access across machines and media for high-bandwidth, low-latency reads and writes. On Kunpeng hardware, host_urma (K5) is supported. host_shm enables shared-memory communication on the same node.
- **Scaling**: LocalService instances can join and leave dynamically.
- **High availability**: In Kubernetes clusters, MetaService supports active-active operation and metadata recovery, providing best-effort HA.

<div align="center">
  <img src="./docs/zh/source/memcache_architecture.png" alt="architecture">
</div>

## 🧩 Core Components

MemCache consists of two core components: LocalService and MetaService.

- **MetaService**：
  - Manages memory-pool allocation across the cluster and handles LocalService join and leave events.
  - Runs as a standalone process and can be started through the Python API or as a binary. See [wheel installation and usage](./docs/zh/install_whl.md) and [run-package installation and usage](./docs/zh/install_run.md).
  - Supports two deployment modes:
  ***1. Standalone mode***: A single MetaService process is easy to deploy, but creates a single point of failure. If the process crashes or becomes unreachable, the system cannot provide service until it recovers.
  ***2. HA mode***: Uses Kubernetes ClusterIP Services and Lease resources. This more complex deployment runs multiple MetaService instances for active-active high availability. See [how to deploy a MemCache HA cluster](https://gitcode.com/Ascend/memcache/wiki/%E6%80%8E%E4%B9%88%E9%83%A8%E7%BD%B2%E4%B8%80%E4%B8%AAmemcache%E7%9A%84HA%E9%9B%86%E7%BE%A4.md).

- **LocalService**: Provides the following functions:
  - **Client**: Exposes APIs to application processes through a shared library distributed as a wheel or `.so` file.
  - **Memory provider**: Provides a contiguous memory region for the memory pool. Other LocalService instances can access it directly by address.

### 📁 Project Structure

MemCache's core code follows a modular design, with the main implementation under `src/memcache/csrc`.

```text
src/memcache/csrc/                        # C++ core implementation
  ├── meta_service/                       # MetaService: cluster metadata, allocation, and eviction
  │    ├── mmc_meta_service.cpp           # Main MetaService logic
  │    ├── mmc_meta_manager.cpp           # Metadata manager (global key-to-blob map)
  │    ├── mmc_blob_allocator.cpp         # Memory-pool allocator
  │    ├── mmc_meta_container_lru.cpp     # LRU metadata container (temperature tracking and eviction)
  │    └── mmc_meta_net_server.cpp        # RPC network server
  ├── client/                             # Local client loaded by inference processes as a wheel/shared library
  │    ├── mmc_client_default.cpp         # Core client implementation (batch put/get/copy)
  │    ├── mmc_meta_net_client.cpp        # RPC network client
  ├── local_service/                      # LocalService: memory provider
  │    ├── mmc_local_service_default.cpp  # Main LocalService implementation
  │    └── mmc_bm_proxy.cpp               # MemFabric BM semantic proxy
  └── under_api/                          # Foundation integration layer (MemFabric / UBSIO)
       ├── mf_smem/                       # MemFabric shared-library integration (HBM / DRAM tiers)
       │    └── smem_bm_api.cpp           # Batch memory-copy API
       └── ubs_io/                        # UBSIO shared-library integration (SSD tier)
            ├── mmc_ubs_io_proxy.cpp      # UBSIO proxy wrapper
            └── dl_ubsio_api.cpp          # Dynamic loading of UBSIO
```

## 🔥 Performance

MemCache provides large memory pools and high-performance H2D, D2H, **D2RH, and RH2D** data access. Because MemCache uses [MemFabric](https://gitcode.com/Ascend/memfabric_hybrid) as its pooling foundation, it supports OneCopy direct data access across machines and media, including RH2D and D2RH. The figure below compares RH2D with other transfer paths.
<div align="center">
  <img src="./docs/zh/source/memcache_rh2d.png" alt="RH2D">
</div>

Based on OneCopy direct access across machines and media, the following performance tests were run on A2/A3.
The test simulates KV-sized blocks for the DeepSeek-R1 model. Each block is 61×128K + 61×16K = 8784KB ≈ 8.57MB across 122 discontiguous addresses.

- Read/write performance using a two-node memory pool with two Ascend A2 nodes (8 cards per node):

<div align="center">
  <img src="./docs/zh/source/memcache_a2.png" alt="A2 perf">
</div>

- Read/write performance using a two-node memory pool with two Ascend A3 nodes (8 cards and 16 dies per node):

<div align="center">
  <img src="./docs/zh/source/memcache_a3.png" alt="A3 perf">
</div>

## 🚀 Quick Start

See the following guides to get started:

- Installation and usage: [install and use the wheel](./docs/zh/install_whl.md) (Python) or [build, install, and use the run package](./docs/zh/install_run.md) (C++).
- [Configuration](docs/zh/memcache_config.md): Shared MetaService and LocalService configuration.
- [Running examples](./example/examples.md): End-to-end C++ and Python examples.
- [Remote development with DevContainer](./docs/zh/devcontainer_quickstart.md): Set up VS Code Remote-SSH and DevContainer, then run all examples.

## 📑 Tutorials

- [C++ API](docs/zh/memcache_c++_api.md): C++ API overview and API list.
- [Python API](docs/zh/memcache_python_api.md): Python API overview and API list.
- [RESTful API](docs/zh/memcache_restful_api.md): Monitoring APIs and API list.

## 📦 Software and Hardware Compatibility

- MemCache depends on [MemFabric](https://gitcode.com/Ascend/memfabric_hybrid), so its compatibility requirements follow MemFabric. See the [complete software and hardware compatibility guide](./docs/zh/compatibility.md) for supported operating systems, Ascend/Kunpeng hardware, CANN versions, MemFabric versions, huge-page sizes, and other requirements.

## 📌 FAQ

See the [FAQ](https://gitcode.com/Ascend/memcache/wiki/FAQ.md) for common questions.

## 📝 Related Information

- [Contributing Guide](./CONTRIBUTING.md)

- [Code of Conduct](./CODE_OF_CONDUCT.md)

- [Security Notes](./docs/zh/SECURITYNOTE.md)

- [License](./LICENSE)

## Note

This open-source project is not a Huawei product and provides limited support.
