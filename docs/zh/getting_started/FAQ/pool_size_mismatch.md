# 不同节点池化 Size 不一致导致启动失败

## 问题现象

不同节点池化Size不一致，多个local进程启动失败，Mmap失败，初始化时日志中有如下打印：

```ini
AddVaInfo failed: address overlap
```

## 问题根因

1、每个local进程的配置参数 `ock.mmc.local_service.max.dram.size` 和 `ock.mmc.local_service.max.hbm.size` 不一致，比如A进程的 `ock.mmc.local_service.max.dram.size` 设置 1GB，B进程的 `ock.mmc.local_service.max.dram.size` 设置为 2GB，则不一致。

2、`ock.mmc.local_service.max.dram.size` 和 `ock.mmc.local_service.max.hbm.size` 没有设置或者设置为0；而不同local进程的 `ock.mmc.local_service.dram.size` 和 `ock.mmc.local_service.hbm.size` 配置不一样，比如A进程`ock.mmc.local_service.hbm.size`设置为1GB，B进程`ock.mmc.local_service.hbm.size`设置为2GB或者为设置，则不一致。

## 解决方案

1、手动设置 `ock.mmc.local_service.max.dram.size`和 `ock.mmc.local_service.max.hbm.size`，让每个local进程一致。

2、如果不设置 `ock.mmc.local_service.max.dram.size` 或者 `ock.mmc.local_service.max.hbm.size`，则每个local进程对应的 `ock.mmc.local_service.dram.size` 和 `ock.mmc.local_service.hbm.size`要一致。
