# 常见问题及解决办法

> [!TIP] 排查步骤
>
> 1. 请先根据下方「问题列表」点击进入对应 FAQ 页面查找解决办法。
> 2. 如果在本文档中未找到对应问题，请前往 [MemFabric Hybrid FAQ](https://memfabric-hybrid.readthedocs.io/zh-cn/latest/wiki/FAQ/) 继续排查。MemCache 的内存池与传输能力依赖 MemFabric Hybrid 底座，该文档按传输协议汇总了内存分配、设备打开、内存注册与导入等底层问题的定位方法，日志中出现 `[HYBM]`、`[SMEM]`、`[HCOM]` 等底层模块报错时尤其适用。

## 问题列表

1. [HalMemCreate 失败（ret: 6）](halmemcreate.md)：内存不足或内核版本不兼容导致内存申请失败。
2. [store 对象初始化失败](store_init_failed.md)：HCOM 获取 gid 失败，`hcom_url` 未正确配置。
3. [HOST_RDMA 跨机传输后出现数据一致性问题](host_rdma_data_consistency.md)：RDMA 网卡 MTU 未大于 4096。
4. [不同节点池化 Size 不一致导致启动失败](pool_size_mismatch.md)：日志报 `AddVaInfo failed: address overlap`。
5. [HA 模式下 master 节点频繁切换](ha_master_switch.md)：续约失败、时钟不同步或租约被外部清理。
6. [多 Python 环境混用 memcache 与 memfabric](python_env_mismatch.md)：版本与 `LD_LIBRARY_PATH` 不一致导致拉起或存取失败。
7. [推理过程中报 lease 过期](lease_expired.md)：单次 batch copy 数据过大超过租约时间。
8. [内存池初始化报错 AutoRanking failed](auto_ranking_failed.md)：集群 rank 总数超过 `world_size` 配置。
