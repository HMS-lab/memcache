# store 对象初始化失败

## 问题现象

调用store初始化接口时，HCOM获取gid失败，日志中报以下错误：

```ini
ERROR [HCOM rdma_device_helper.cpp:322] Failed to get proper gid by address for ip 127.0.0.1
ERROR [HCOM net_rdma_driver.cpp:188] Failed to get device by ip
ERROR [HCOM net_rdma_driver.cpp:60] RDMA failed to create ctx
INFO [HCOM service_periodic_manager.cpp:189] PeriodicManager for timeout [name: hybm_hcom_service, index: 0] working thread exit
[HCOM service_imp.cpp:151] Driver start failed, res:210
ERROR [952-952][HYBM host_hcom_transport_manager.cpp:115 OpenDevice] Failed to start hcom service, nic: tcp://127.0.0.1:8100 type: 0 ret: 210
ERROR [952-952][HYBM compose_transport_manager.cpp:59 OpenDevice] Failed to open device transport nic: tcp://127.0.0.1:8100
ERROR [952-952][HYBM host_hcom_transport_manager.cpp:135 CloseDevice] Assert rpcService_ != 0
ERROR [952-952][HYBM hybm_entity_default.cpp:1048 InitTransManager] Failed to open device, ret: -1
ERROR [952-952][HYBM hybm_entity_default.cpp:96 Initialize] Failed to InitTransManager
ERROR [952-952][HYBM hybm_big_mem_entry.cpp:34 hybm_create_entity] initialize entity failed: -1
ERROR [952-952][SMEM smem_bm_entry.cpp:54 Initialize] create entity failed
ERROR [952-952][SMEM smem_bm.cpp:202 smem_bm_create2] entry init failed, result: -1
ERROR [952-952][MMC mmc_bm_proxy.cpp:103 InternalCreateBm] Failed to create smem bm
ERROR [952-952][MMC mmc_bm_proxy.cpp:60 InitBm] Internal create bm failed
INFO [952-952][HYBM hybm_entry.cpp:134 hybm_uninit] uninitialize GVA memory return: 0
INFO [952-952][SMEM smem_bm.cpp:105 smem_bm_uninit] smem_bm_uninit finished
ERROR [952-952][MMC mmc_local_service_default.cpp:38 Start] Failed to init bm of local service local_service, error code -1
ERROR [952-952][MMC mmc_service.cpp:53 mmcs_local_service_start] create or start local service failed
ERROR [952-952][MMC mmc.cpp:70 mmc_init] failed to create or start local service
```

## 问题根因

此问题是因为mmc-local.conf配置文件中将ock.mmc.local_service.protocol设置为host_rdma，但是没有正确设置ock.mmc.local_service.hcom_url中的ip地址

## 解决方案

需要在环境上执行show_gids查询RDMA网卡的IP地址，并将其设置到ock.mmc.local_service.hcom_url中
