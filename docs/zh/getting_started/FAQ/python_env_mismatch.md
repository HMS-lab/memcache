# 多 Python 环境混用 memcache 与 memfabric

## 问题现象

环境中有多个python环境时混用memcache和memfabric，导致无法拉起或者存取失败。推理框架服务无法拉起或者存取失败，报类似下列的错误：

```ini
unpack response body failed, result: -1
```

或者打印的so library版本与安装的memcache和mf不匹配：

```ini
hybm init successfully,  library version: xxx"
```

## 问题根因

此问题是因为推理框架使用的python版本和export LD_LIBRARY_PATH指定的memcache和memfabric的路径不一致，导致无法拉起或者存取失败

## 解决方案

检查安装的memcache和memfabric版本和export LD_LIBRARY_PATH指定的memcache和memfabric的路径是否一致，同时检查推理框架脚本和memcache metaservice启动脚本
不一致则改成相同的版本
