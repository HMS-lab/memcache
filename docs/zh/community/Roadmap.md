# <span style="color:#008a00;">MemCache Roadmap 2026 H1</span>

MemCache在2026年H1阶段的目标是增强在多级缓存方面的能力、持续提升集群可靠性和易用性，具体的研发Roadmap如下：

---

## 关键特性

### <span style="color:#0066cc;">多级缓存能力</span>

- [ ] [**630**] 支持HBM+DRAM+SSD三层缓存，已提供POC测试版本，630推出新架构版本
- [x] [**2025H4**] 支持HBM + DRAM两级缓存
- [x] [**2025H3**] 支持DRAM单级缓存

### <span style="color:#0066cc;">集群可靠性</span>

- [x] [**2025H4**] 支持Meta Service高可用，支持主备切换和元数据恢复
- [x] [**2025H4**] 支持Local Service动态扩缩，支持数据多副本放置策略

### <span style="color:#0066cc;">可维测</span>

- [x] [**130**] Meta Service支持基于[restful](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/memcache_restful_api.md)进行集群管理、数据查询和指标查询，支持对接Prometheus和Grafana

---
