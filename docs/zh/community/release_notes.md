# 版本说明书

## 版本配套说明

### 产品版本信息

| 项目 | 内容 |
| ---- | ---- |
| 产品名称 | memcache |
| 产品版本 | develop |
| 版本类型 | 正式版本 |
| 维护周期 | 三个月 |

### 相关产品版本配套说明

**硬件版本配套表**

| 产品名称 | 版本 |
| --- | --- |
| 服务器 | <ul><li> Atlas 200T A2 Box16 </li><li> Atlas 800I A2/A3 系列产品 </li><li> Atlas 800T A2/A3 系列产品 </li><li> Atlas 900 A3 SuperPoD </li></ul> |
| 平台 | <ul><li>aarch64</li> <li>x86</li> </ul>|

**软件版本配套表**

| 产品名称 | 版本 |
| --- | --- |
|OS|Ubuntu 22.04 LTS+|
|cmake| 3.20.x|
|gcc |11.4+|
|python| 3.11.10|
|pybind11| 2.10.3|
|make |4.3|
|ninja| 1.10.1|

## 版本兼容性说明

无

## 更新说明

### 关键特性变更

- MemFabric支持openclaw场景，支持长序列，Prefill吞吐提升5倍。
- MemFabric支持A5 UB高速总线，支持基于UDMA的D2D/H2H/RH2D/RD2H等场景，4平面单D跨节点带宽达成200GB/s。
- MemFabric支持A5，适配A5卡/服务器/超节点，容量支持128TB以上，LLM场景Prefill吞吐相比软件使能提升5倍，LLM场景TTFT相比软件使能前优化80%。A3上既有能力平滑迁移到A5。
- Memcache支持KV Cache亲和调度。
- 支持Prefill阶段分层KV Cache加载提升序列长度。
- 内存池化支持decode侧DSA，支持decode cache卸载到本机DDR。
- 内存池化支持SSD，实现超大容量内存支持AgenticAI。
- 内存池化支持下层网络提供QoS能力，解决流量冲突问题，确保流量冲突时高优业务优先通过。

### 接口变更说明

无

### 已解决的问题

无

### 遗留问题

无

## 升级影响

### 升级过程中对现行系统的影响

- 对业务的影响

    软件版本升级过程中会导致业务中断。

- 对网络通信的影响

    对通信无影响。

### 升级后对现行系统的影响

无

## 版本配套文档

|文档名称|内容简介|
|---|---|
|《[run 安装](../install_run.md)》|介绍run包的编译、安装及使用方法，适用于需要使用C++接口的用户。|
|《[whl 安装](../install_whl.md)》|介绍whl包的安装及使用方法，适用于需要使用Python接口的用户。|
|《[C 接口](../memcache_c_api.md)》|C接口介绍以及对应的API列表。|
|《[C++ 接口](../memcache_c++_api.md)》|C++接口介绍以及对应的API列表。|
|《[Python 接口](../memcache_python_api.md)》|Python接口介绍以及对应的API列表。|
|《[RESTful 接口](../memcache_restful_api.md)》|提供对外的RESTful接口。|
|《[安全说明](../SECURITYNOTE.md)》|提供了MemCache安全配置相关的内容。|
|《[DevContainer 远端开发](../devcontainer_quickstart.md)》|VS Code Remote-SSH + DevContainer 一站式开发环境搭建与全量示例运行指南。|
|《[C/C++ 编码规范](../c_cpp_naming.md)》|提供C/C++的编码规范。|
|《[配置文件](../memcache_config.md)》|提供MetaService、LocalService的配置参数说明。|
|《[样例执行](../../../example/examples.md)》|介绍如何端到端执行样例代码，包括C++和Python样例。|
