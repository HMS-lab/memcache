# KvEvent设计文档

***

**作者 (Authors):** @Ascend/Memcache<br>
**创建日期 (Created):** 2026-07-13<br>
**更新日期 (Updated):** 2026-07-13<br>
**相关 Issue/PR:**

- <https://gitcode.com/Ascend/memcache/pull/334>
- <https://gitcode.com/Ascend/memcache/pull/363>

***

# 1. 概述

## 1.1 简介

kvevent广播写操作事件。在元数据写入或删除时通过ZMQ PUB发送事件到订阅者。此功能对接PyMotor kv-conductor计算kv亲和度以使能缓存感知prefill调度。

## 1.2 动机

现状：Agentic AI 多轮对话动辄数十万 Token，请求间共享大量相同前缀（System Prompt、历史消息、RAG 知识库）。

痛点： 跨请求随机调度导致 KV Cache 无法命中，每轮重复 Prefill 全量上下文，造成 TTFT 高企与算力严重浪费。

解决方案：缓存感知调度。精准识别最长前缀命中，智能路由请求至最优节点，最大化 KV 复用。

跨请求 KV 复用：基于 Memcache 池化存储保存已算 KV，后续请求直接命中，消除重复 Prefill。

## 1.3 目标

目标：赋能PyMotor kv池状态感知，使能缓存感知prefill调度，最大化KV复用，降低TTFT。

非目标：kvevent仅广播事件，不做调度、编排。性能提升依赖PyMotor调度kv复用度。

# 2. 用例分析（需求分析）

| 用例     | 优先级 | 性能目标 / 功能目标                          | 备注                                                     |
| ------ | --- | ------------------------------------ | ------------------------------------------------------ |
| 缓存状态感知 | P0  | 开启kvevent广播功能后memcache性能不劣化          | ZMQ以异步队列+DONTWAIT方式实现，不阻塞功能主路径                         |
| HA主备切换 | P1  | 主备切换时事件发布不丢、不乱                       | SetActive(false)停写队列、SetActive(true)恢复，HA模式下默认inactive |
| 状态信息   | P2  | 提供Restful API返回事件发送量、丢弃量等统计信息，提升可维测性 | KvEventStats覆盖published/dropped/skipped等维度             |

# 3. 方案设计

## 3.1 总体方案

在元数据写入/删除操作成功后，通过ZMQ PUB socket异步广播kv事件。整体架构如下：

```text
写入成功 → MmcKvEventRuntime.OnMetaStored/OnMetaRemoved
               ↓
         KvEventPublisher.Enqueue (异步队列, mutex保护)
               ↓
         WorkerLoop 线程 → 批量取出 → BuildEvent + SerializeEventBatch (msgpack)
               ↓
         IKvEventTransport.Send (ZMQ PUB, ZMQ_DONTWAIT)
               ↓
         ZMQ订阅者 (kv-conductor) ← 三帧: topic | seq(BE64) | payload(msgpack)
```

关键设计约束：

- **非阻塞**：调用方线程仅入队后立即返回，ZMQ发送使用`ZMQ_DONTWAIT`避免worker线程阻塞。
- **优先级**：REMOVED/CLEARED为高优先级事件，队列满时可挤占STORED位置，确保删除事件不丢。
- **HA适配**：备节点默认`active=false`，主备切换时通过SetActive动态启停发布。

## 3.2 技术选型

| 方案          | 优势                               | 劣势              | 决策     |
| ----------- | -------------------------------- | --------------- | ------ |
| ZMQ PUB/SUB | 成熟稳定、零拷贝发送、天然支持多订阅者、LINGER=0快速关闭 | 无内置重试、需管理SNDHWM | **选用** |

### 加密通讯方案

| 方案           | 优势                                       | 劣势                          | 决策     |
| ------------ | ---------------------------------------- | --------------------------- | ------ |
| TLS over TCP | 企业级安全标准、证书生态成熟                           | 需要额外的证书管理、握手开销大、不适合嵌入式ZMQ场景 | 放弃     |
| 应用层加密        | 灵活可控                                     | 实现复杂、需要自行管理密钥协商             | 放弃     |
| ZMQ CURVE    | ZMQ官方主推加密方案、内置于libzmq、零额外依赖、NaCl加密算法、轻量级 | 仅支持加密和认证，不支持复杂的权限控制         | **选用** |

**ZMQ CURVE 加密通讯：**

CURVE 是 ZMQ 官方推荐的加密通讯机制，基于 NaCl 加密库，提供端到端加密和可选的客户端认证。

- **加密算法**：Curve25519（密钥交换）+ XSalsa20-Poly1305（对称加密）
- **密钥格式**：Z85 编码的 40 字符字符串（32 字节二进制）。密钥对必须通过 ZMQ 原生接口 (zmq_curve_keypair()) 或底层 libsodium 库生成。这些接口内部调用操作系统的密码学安全伪随机数生成器 (CSPRNG)，确保密钥具备不可预测的高熵值。
离线生成原则：密钥的生成过程在部署前的离线环境中完成，严禁在运行时动态生成或使用弱随机源。
- **认证模式**：支持 server-only 认证（仅加密）或 server+client 双向认证（需 ZAP 协议）
- **官方文档**：<https://libzmq.readthedocs.io/en/latest/zmq_setsockopt.html>

本项目采用 server-only 认证模式，仅加密通讯，不做客户端认证。若启用加密但密钥配置异常，系统自动降级为未加密模式并输出 ERROR 日志。

## 3.3 功能与性能设计

### 事件类型

| 事件      | KvEventType | 含义            | 优先级 |
| ------- | ----------- | ------------- | --- |
| STORED  | `STORED`    | KV块写入完成       | 普通  |
| REMOVED | `REMOVED`   | KV块删除完成       | 高   |
| CLEARED | `CLEARED`   | 指定nd/rank全量清除 | 高   |

### 核心数据流

1. **入队阶段**：调用方线程加锁入队。STORED在队列达`queueCapacity`时直接丢弃；REMOVED/CLEARED在队列达`2×queueCapacity`（HardQueueCapacity）时先挤占STORED，仍满则丢弃并计入`droppedHighPriorityEvents`。
2. **发送阶段**：Worker线程批量取出（maxBatchSize），逐事件BuildEvent（提取blockHash、填充元数据），msgpack序列化后ZMQ发送。发送失败时计入`droppedEvents`/`droppedStoredEvents`/`droppedHighPriorityEvents`。
3. **ZMQ帧格式**：三帧多部分消息 — `topic | seq(BE uint64) | payload(msgpack map array)`。

### KvEvent字段

| 字段               | 类型                    | 适用事件类型           | 说明                                 |
| ---------------- | --------------------- | ---------------- | ---------------------------------- |
| event\_id        | uint64                | common           | (medium, backendId)维度单调递增          |
| timestamp        | uint64                | common           | 毫秒级Unix时间戳                         |
| event\_type      | string                | common           | "stored"/"removed"/"cleared"       |
| model\_name      | string?               | common           | 模型名称                               |
| block\_size      | uint32?               | common           | KV块大小                              |
| additional\_salt | string?               | common           | 额外盐值                               |
| lora\_name       | string?               | common           | LoRA名称                             |
| tenant\_id       | string                | common           | 租户ID，默认"default"                   |
| medium           | string?               | common           | "xpu"(HBM)/"cpu"(DRAM)/"disk"(SSD) |
| backend\_id      | string                | common           | 目标backend标识                        |
| dp\_rank         | uint32?               | common           | 计算dp rank（当前留空）                    |
| seq\_hashes      | uint64\[] 或 string\[] | stored + removed | blockHash列表，由objectKey解析           |
| base\_block\_idx | nil                   | stored + removed | 占位字段，当前输出nil                       |
| parent\_hash     | nil                   | stored only      | 占位字段，当前输出nil                       |
| token\_ids       | nil                   | stored only      | 占位字段，当前输出nil                       |

> **适用事件类型**：common = STORED/REMOVED/CLEARED均有；stored + removed = 仅STORED和REMOVED（CLEARED在seq\_hashes后直接return）；stored only = 仅STORED。

### 性能约束

- 入队操作：O(1)加锁入队，不影响写入热路径。
- ZMQ发送：`ZMQ_DONTWAIT`非阻塞，SNDHWM=10000，满则返回EAGAIN并计入dropped统计。
- 析构安全：`ZMQ_DONTWAIT`保证worker线程不阻塞于send，`stop_`标志+`join()`可靠完成；`ZMQ_LINGER=0`确保socket关闭时丢弃未发送帧。

## 3.4 安全隐私与DFX设计

兼容性：模块化设计，kvevent代码独立，仅在写入成功时插入广播代码。配置项`mmc_kv_events_config_t`作为`mmc_meta_service_config_t`子结构体，默认`enable=false`，不影响存量配置。

可维护性：Transport以`IKvEventTransport`抽象基类方式，仅暴露`Send(seq, payload)`接口，方便未来支持其他广播方式（如HTTP、gRPC）。

可测试性：代码仓流水线继承UT并跟踪测试覆盖率（行覆盖率>=70%，分支覆盖率>=40%）。UT通过mock transport验证入队、丢包、批量发送、HA切换等场景。

可靠性：

- 入队丢包有计数统计（`droppedEvents`/`droppedStoredEvents`/`droppedHighPriorityEvents`）。
- 发送失败有计数统计（同上，Send返回false时累加）。
- `MmcMetaChangeCallbacks`读写通过`std::mutex`保护，消除Stop()期间回调竞争写入UB。
- ZMQ `DONTWAIT`避免worker阻塞导致析构挂死。

## 3.5 编程与调用设计

### 3.5.1 编程模型基本设计

\*\*开发环境设计：\*\*同Memcache主项目（C++17、CMake >= 3.12、Ninja/Make）。

**开发约束：**

- kv\_event模块仅内部使用，不暴露为公共C/Python API。
- 编译宏`MMC_ENABLE_KV_EVENTS`控制功能编译，未定义时`MmcKvEventRuntime`全部no-op。

**可验收设计：**

- 功能验收：启动meta service配置`kvEvents.enable=true`，订阅端收到STORED/REMOVED/CLEARED事件。
- 性能验收：开启kv\_events后memcache基准性能不低于关闭时（入队O(1)、ZMQ DONTWAIT不阻塞）。
- 可维测验收：通过Restful API获取`KvEventStats`，验证published/dropped计数正确。

### 3.5.2 配置说明

KV事件广播功能通过MetaService配置文件开启，支持以下配置项：

| 配置项                                           | 类型     | 默认值       | 说明                                                                   |
| --------------------------------------------- | ------ | --------- | -------------------------------------------------------------------- |
| ock.mmc.kv\_events.enable                     | bool   | false     | 是否开启KV事件广播功能                                                         |
| ock.mmc.kv\_events.endpoint                   | string | ""        | ZMQ PUB绑定地址，格式如 `tcp://127.0.0.1:5557`                               |
| ock.mmc.kv\_events.model\_name                | string | ""        | 模型名称，用于构建kv\_event envelope                                          |
| ock.mmc.kv\_events.tenant\_id                 | string | "default" | 租户ID，用于构建kv\_event envelope                                          |
| ock.mmc.kv\_events.block\_size                | uint32 | 0         | KV块大小，用于构建kv\_event envelope                                         |
| ock.mmc.kv\_events.queue\_capacity            | uint32 | 65536     | 内部队列软容量，硬容量为2倍此值                                                     |
| ock.mmc.kv\_events.hash\_as\_int              | bool   | true      | seq\_hashes编码方式：true输出uint64数组，false输出hex字符串数组                       |
| ock.mmc.kv\_events.curve\_enable              | bool   | false     | （开发中）是否启用ZMQ CURVE加密通信。启用后需配置server\_public\_key和server\_secret\_key |
| ock.mmc.kv\_events.curve\_server\_public\_key | string | ""        | （开发中）ZMQ CURVE server公钥（Z85编码，40字符）。启用加密时必填                          |
| ock.mmc.kv\_events.curve\_server\_secret\_key | string | ""        | （开发中）ZMQ CURVE server私钥（Z85编码，40字符）。启用加密时必填，属于敏感信息                   |

**加密通信说明：**

- 启用 `curve_enable=true` 后，ZMQ PUB socket 将使用 CURVE 加密协议，所有事件数据在传输过程中加密。
- 若启用加密但未配置公钥/私钥，或密钥格式不正确，系统将自动降级为未加密模式，并输出ERROR日志提醒。
- 订阅端（如 kv-conductor）需要配置 server 的公钥才能解密接收事件。公钥需通过安全渠道线下分发给订阅端。
- 密钥生成方式：使用 `zmq_curve_keypair()` 函数或 `zmqcert` 工具预先生成 Z85 编码的密钥对。

### 3.5.3 编程手册设计

在已有`doc/memcache_config.md`中新增`kv_events`配置项说明，在`doc/memcache_restful_api.md`中新增`/stats/kv_events`查询接口说明。不单独输出手册。

# 4. 缺点和风险

| 风险             | 影响                                     | 应对措施                                                                    |
| -------------- | -------------------------------------- | ----------------------------------------------------------------------- |
| ZMQ订阅者断连/慢消费   | SNDHWM满后事件丢弃，kv-conductor亲和表可能短暂缺失     | DONTWAIT+dropped统计可观测；可调低SNDHWM减少陈旧积压                                   |
| HA备节点不发布       | 备节点期间事件不广播，主切换后需重新建立亲和                 | SetActive(true)后立即恢复发布，kv-conductor应容忍短暂空窗                              |
| objectKey格式不兼容 | seq\_hashes解析失败导致skippedUnparsedKeys增长 | skippedUnparsedKeys统计可观测；ExtractBlockHashFromObjectKey需与上层objectKey格式同步 |

# 5. 现有技术

| 项目/社区                   | 设计                                       | 本项目借鉴                    | 差异                                               |
| ----------------------- | ---------------------------------------- | ------------------------ | ------------------------------------------------ |
| vLLM kv event publisher | ZMQ PUB广播KV块增删事件，msgpack编码，seq\_hashes字段 | 事件数据模型、msgpack序列化格式、字段命名 | vLLM单进程内直接PUB，无加密；本项目跨MetaService进程，增加异步队列+批量+优先级+HA+CURVE加密 |
| Redis Pub/Sub           | 通道发布/订阅，支持多消费者                           | 一对多广播模型                  | Redis需额外部署；本项目ZMQ嵌入式、零依赖                         |
| Kafka                   | 分布式消息队列，持久化、重试                           | 无                        | Kafka过重，不需要持久化/重放，ZMQ满足低延迟场景                     |

# 6. 未解决问题

1. 通讯加密开发中。

***

附录

- **参考资料：** vLLM kv-event实现、ZMQ PUB/SUB文档、msgpack规范
- **术语表：**
  - kv-conductor：PyMotor侧KV亲和度计算服务
  - TTFT：Time To First Token
  - SNDHWM：ZMQ Send High Water Mark，发送端最大缓冲消息数
  - DONTWAIT：ZMQ非阻塞发送标志
- **文档更新计划：** 同步更新`doc/memcache_config.md`（kv\_events配置项）、`doc/memcache_restful_api.md`（stats查询接口）
