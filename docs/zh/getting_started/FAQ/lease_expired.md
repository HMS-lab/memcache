# 推理过程中报 lease 过期

## 问题现象

推理框架服务在推理过程中，取kvcache失败，报类似下列的错误

```ini
batch get lease expired.
batch copy read lease expired
lease expired after read.
```

## 问题根因

此问题是因为，在读取数据的时候，先往meta发起读取元数据，meta会记录此刻的实际和一个租约时间（默认10s），这个租约时间表示如果后续读数据超过了租约时间，则会报上面的租约过期。如果单次batch copy的数据过大，可能会触发这个问题。

## 解决方案

通过修改meta的配置项：ock.mmc.meta.lease_ttl_ms，适当调大即可解决，比如设置 50000。
