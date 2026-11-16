# 内存池初始化报错 AutoRanking failed

## 问题现象

推理框架服务拉起过程中，报如下错误

```ini
2026-07-08 16:20:08.129923 ERROR [1757-1757][SMEM smem_bm_entry_manager.cpp:152 AutoRanking] AutoRanking failed, deviceId: 0, ret: -1, dataSize: 0
2026-07-08 16:20:08.129934 ERROR [1757-1757][SMEM smem_bm_entry_manager.cpp:78 Initialize] [RECOVER] Initialize attempt 5/5 failed, ret=-1 deviceId=0
2026-07-08 16:20:08.129939 ERROR [1757-1757][SMEM smem_bm_entry_manager.cpp:86 Initialize] initialize failed: -1
2026-07-08 16:20:08.130005 ERROR [1757-1757][SMEM smem_bm.cpp:86 smem_bm_init] init bm entry manager failed, result: -1
2026-07-08 16:20:08.130018 ERROR [1757-1757][MMC mmc_bm_proxy.cpp:55

```

## 问题根因

此问题是因为集群rank总数大于mmc_local.conf中设置的 ock.mmc.local_service.world_size 值（默认为 256），rank 数按（总使用的卡数 + 分离部署进程数）计算。

## 解决方案

通过修改mmc_local.conf中 ock.mmc.local_service.world_size 的配置项，需要大于等于总rank数
