---
hide:
  - navigation
---

# MemCache

MemCache 是面向大模型推理场景的 KVCache 分级缓存组件。它将 HBM 之外的 DRAM、SSD 等内存资源组织为统一的缓存池，通过多级池化扩大 KVCache 容量、提升命中率，有效降低 TTFT（首 token 响应时延）并缓解算力压力。

核心组件：

- **MetaService** — 集群元数据管理与内存池调度
- **LocalService** — 节点内存池服务，提供 DRAM/HBM/SSD 分级存储

数据传输由独立配套组件 **MemFabric** 提供，支持 HCCS/SDMA/RDMA 等多种互联方式。

整体架构如下：

![MemCache 架构图](source/memcache_architecture.png)

请使用顶部导航栏浏览 [快速开始](getting_started/index.md)、[用户指南](user_guide/feature_guide/DRAM+SSD多级池化/特性介绍.md)、[开发者指南](developer_guide/design_documents/KvEvent设计文档.md)与[社区](community/index.md)。
