# 安全提示

## 通信矩阵

| 原设备             | 源IP地址     | 源端口                                  | 目的设备            | 目的IP地址                    | 目的端口（侦听）                                               | 协议                 | 端口说明                                                    | 侦听端口是否可更改 | 认证方式              |
| --------------- | --------- | ------------------------------------ | --------------- | ------------------------- | ------------------------------------------------------ | ------------------ | :------------------------------------------------------ | --------- | ----------------- |
| Local/Client客户端 | 客户端通信IP地址 | 随机端口（由操作系统自动分配）                      | meta service    | meta\_service\_url中的\<ip> | meta\_service\_url中的\<port>，默认值5000，可配范围\[1025, 65535] | TCP                | 用于元数据对象管理                                               | 是         | TLS               |
| Local/Client客户端 | 客户端通信IP地址 | 随机端口（由操作系统自动分配） | meta service（etcd HA模式） | 由 memfabric 通过 config store 发布的 bindable leader 地址 | 动态分配（MetaService 侦听端口），范围\[9000, 65535]，可用 `MEMFABRIC_HYBRID_CONFIG_STORE_PORT_START/END` 收窄 | TCP | 用于元数据对象管理；etcd HA 下 `meta_service_url` 仅作配置值，实际侦听端口由 memfabric 分配 | 否（由 memfabric 分配） | TLS |
| memory fabric实例 | 客户端通信IP地址 | 随机端口（由操作系统自动分配） | config store（etcd HA Leader） | 由 memfabric 写入 etcd 的 leader 地址 | 动态分配（config store 侦听端口），范围\[9000, 65535]，与 MetaService 动态端口同区间且互不相同 | TCP | 用于 memory fabric 中 BM 信息交换同步；etcd HA 下 `config_store_url` 指向 etcd，实际 Leader 侦听端口由 memfabric 分配 | 否（由 memfabric 分配） | TLS |
| memory fabric实例 | 客户端通信IP地址 | 随机端口（由操作系统自动分配）                      | memory fabric实例 | config\_store\_url中的\<ip> | config\_store\_url中的\<port>，默认值6000，可配范围\[1025, 65535] | TCP                | 用于memory fabric中BM信息交换同步                                | 是         | TLS               |
| 参与hcom通信的实例     | 客户端通信IP地址 | 随机端口（由操作系统自动分配）                      | 参与hcom通信的实例     | hcom\_url中的\<ip>          | hcom\_url中的\<port>，默认值7000，可配范围\[1024, 65535]          | TCP/RDMA/SDMA      | 用于hcom通信；多 local_service 时实际侦听端口为 基准端口 + local_rank，见下表注 | 是         | TLS               |
| 管理/监控客户端        | 客户端通信IP地址 | 随机端口（由操作系统自动分配）                      | meta service    | metrics\_url中的\<ip>       | metrics\_url中的\<port>，默认值8000，可配范围\[1025, 65535]       | HTTPS/HTTP         | 用于RESTful API管理与监控                                      | 是         | mTLS(可选，默认关闭)/无认证 |
| ZMQ通道发布端        | 客户端通信IP地址 | kv\_events.endpoint中的\<port>，默认不侦听（`kv_events.enable=false`），启用后必填，示例5557 | ZMQ订阅端          | 单向广播，不涉及                  | 单向广播，不涉及                                               | ZMTP（ZeroMQ，基于TCP） | 通过ZMQ通道发送KV事件（kvevent）；解耦架构下该ZMQ通道当前用于承载KV事件推送，生产与订阅方解耦 | 是         | CURVE（可选，默认关闭）    |

- 支持通过配置文件配置TLS私钥、证书、口令等，进行TLS安全连接。
- 支持对 metrics HTTP 服务独立配置 mTLS（双向认证），通过 `ock.mmc.metrics.tls.*` 配置项开启。
- 建议用户开启TLS配置开关，并使用加密的方式保存私钥，保证通信安全。
- 系统启动后，建议删除本地秘钥证书等信息敏感文件。
- 支持通过环境变量 `ACCLINK_CHECK_PERIOD_HOURS`和`ACCLINK_CERT_CHECK_AHEAD_DAYS` 配置证书检查周期与证书过期预警时间。
- 多local_service场景不同local_service之间会使用配置文件中的HCOM端口+local_rank作为实际使用的端口：实际侦听端口范围为 \[基准端口, 基准端口 + world_size - 1]；HCOM 建链失败时会以 +1 递增重试（最多 8 次），最坏占用到 基准端口 + world_size - 1 + 7。因此要求 **基准端口 + world_size - 1 \<= 65535**（即基准端口最大值 = 65535 - (world_size - 1)），超出时 memcache 在配置加载阶段直接报错，不会拖到运行期。
- etcd HA 模式下，Leader 由 memfabric 在默认 \[9000, 65535] 内自动选择**两个互不相同的空闲端口**：一个作为 config store 的侦听端口（leader 地址写入 etcd），另一个作为 MetaService 的侦听端口（bindable leader 地址，供 LocalService/客户端发现后直连）；区间可用 `MEMFABRIC_HYBRID_CONFIG_STORE_PORT_START` / `MEMFABRIC_HYBRID_CONFIG_STORE_PORT_END`（别名 `MF_CONFIG_STORE_PORT_START` / `MF_CONFIG_STORE_PORT_END`）收窄，两个端口均由 memfabric 分配、memcache 侧无法指定，端口范围过窄或被占用会导致竞选失败。

| 环境变量                          | 说明                                                        |
|-------------------------------|-----------------------------------------------------------|
| ACCLINK_CHECK_PERIOD_HOURS    | 指定证书检查周期（单位：小时），超出范围 [ 24, 24 \* 30 ] 或不是整数，则设置默认值 7 \* 24   |
| ACCLINK_CERT_CHECK_AHEAD_DAYS | 指定证书预警时间（单位：天），超出范围 [ 7, 180 ] 或不是整数或换算成小时小于检查周期，则设置默认值30 |

## 运行用户建议

- 基于安全性考虑，建议您在执行任何命令时，不建议使用root等管理员类型账户执行，遵循权限最小化原则。

## 文件权限最大值建议

- 建议用户在主机（包括宿主机）及容器中设置运行系统umask值为0027及以上，保障新增文件夹默认最高权限为750，新增文件默认最高权限为640。
- 建议对使用当前项目已有和产生的文件、数据、目录，设置如下建议权限。

| 类型                | Linux权限参考最大值   |
|-------------------|----------------|
| 用户主目录             | 750（rwxr-x---） |
| 程序文件(含脚本文件、库文件等)  | 550（r-xr-x---） |
| 程序文件目录            | 550（r-xr-x---） |
| 配置文件              | 640（rw-r-----） |
| 配置文件目录            | 750（rwxr-x---） |
| 日志文件(记录完毕或者已经归档)  | 440（r--r-----） |
| 日志文件(正在记录)        | 640（rw-r-----） |
| 日志文件目录            | 750（rwxr-x---） |
| Debug文件           | 640（rw-r-----） |
| Debug文件目录         | 750（rwxr-x---） |
| 临时文件目录            | 750（rwxr-x---） |
| 维护升级文件目录          | 770（rwxrwx---） |
| 业务数据文件            | 640（rw-r-----） |
| 业务数据文件目录          | 750（rwxr-x---） |
| 密钥组件、私钥、证书、密文文件目录 | 700（rwx—----）  |
| 密钥组件、私钥、证书、加密密文   | 600（rw-------） |
| 加解密接口、加解密脚本       | 500（r-x------） |

## 调用acc_links接口列表

### TCP服务端模块

| 接口功能描述       | 接口声明                                                                                                                                                   |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 创建TCP服务端     | `static AccTcpServerPtr Create();`                                                                                                                     |
| 启动服务端        | `int32_t Start(const AccTcpServerOptions &opt);`                                                                                                       |
| TLS认证方式启动服务端 | `int32_t Start(const AccTcpServerOptions &opt, const AccTlsOption &tlsOption);`                                                                        |
| 停止服务端        | `void Stop();`                                                                                                                                         |
| 连接其余服务端      | `int32_t ConnectToPeerServer(const std::string &peerIp, uint16_t port, const AccConnReq &req, uint32_t maxRetryTimes, AccTcpLinkComplexPtr &newLink);` |
| 注册处理新请求事件函数  | `void RegisterNewRequestHandler(int16_t msgType, const AccNewReqHandler &h);`                                                                          |
| 注册处理断链事件函数   | `void RegisterLinkBrokenHandler(const AccLinkBrokenHandler &h);`                                                                                       |
| 注册处理新链接事件函数  | `void RegisterNewLinkHandler(const AccNewLinkHandler &h);`                                                                                             |
| 注册密码解密的函数    | `void RegisterDecryptHandler(const AccDecryptHandler &h);`                                                                                             |
| 加载安全认证所需动态库  | `int32_t LoadDynamicLib(const std::string &dynLibPath);`                                                                                               |

## 依赖软件声明

当前项目运行依赖 CANN 和 Ascend HDK，安装使用及注意事项参考 [CANN](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1beta1/index/index.html) 和 [Ascend HDK](https://support.huawei.com/enterprise/zh/undefined/ascend-hdk-pid-252764743) 并选择对应版本。

## 源码内公网地址

| 类型         | 开源代码地址                                        | 文件名                     | 公网IP地址/公网URL地址/域名/邮箱地址                        | 用途说明          |
| ---------- | --------------------------------------------- | ----------------------- | --------------------------------------------- | ------------- |
| 依赖三方库      | <https://gitcode.com/GitHub_Trending/go/googletest.git> | .gitmodules             | <https://gitcode.com/GitHub_Trending/go/googletest.git> | 单元测试框架依赖      |
| 依赖三方库      | <https://github.com/sinojelly/mockcpp.git>    | .gitmodules             | <https://github.com/sinojelly/mockcpp.git>    | 单元测试对象Mock框架  |
| 依赖三方库      | <https://gitcode.com/GitHub_Trending/sp/spdlog.git> | .gitmodules             | <https://gitcode.com/GitHub_Trending/sp/spdlog.git> | 日志框架依赖        |
| 依赖三方库      | <https://gitcode.com/GitHub_Trending/js/json.git> | .gitmodules             | <https://gitcode.com/GitHub_Trending/js/json.git> | JSON解析与序列化依赖   |
| 依赖三方库      | <https://github.com/biaks/prometheus-cpp-lite.git> | .gitmodules             | <https://github.com/biaks/prometheus-cpp-lite.git> | Prometheus监控指标依赖 |
| 依赖三方库      | <https://gitcode.com/gh_mirrors/ms/msgpack-c.git> | .gitmodules             | <https://gitcode.com/gh_mirrors/ms/msgpack-c.git> | KV事件消息序列化依赖    |
| 依赖三方库      | <https://gitcode.com/gh_mirrors/li/libzmq.git> | .gitmodules             | <https://gitcode.com/gh_mirrors/li/libzmq.git> | ZMQ通信库（KV事件ZMQ通道传输） |
| 依赖三方库      | <https://gitcode.com/gh_mirrors/li/libsodium.git> | .gitmodules             | <https://gitcode.com/gh_mirrors/li/libsodium.git> | ZMQ CURVE加密底层依赖 |
| license 地址 | 不涉及                                           | LICENSE                 | <http://www.apache.org/licenses/>             | license文件       |
| license 地址 | 不涉及                                           | LICENSE                 | <http://www.apache.org/licenses/LICENSE-2.0>  | license文件       |
| 代码仓地址      | <https://gitcode.com/Ascend/memcache>         | setup.py                | <https://gitcode.com/Ascend/memcache>         | whl 包仓库地址信息     |
| 代码仓地址      | <https://gitcode.com/Ascend/memfabric_hybrid> | performance\_compare.sh | <https://gitcode.com/Ascend/memfabric_hybrid> | 工具脚本          |
