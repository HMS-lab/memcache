# 节点亲和分配策略（local\_first）设计文档

**作者 (Authors):** @huawei_zixiqu<br>
**创建日期 (Created):** 2026-10-08<br>
**更新日期 (Updated):** 2026-10-08

***

# 1. 概述

## 1.1 简介

MemCache 现有 `ALLOC_RANDOM` 策略会把一次 put 请求的 keys 按 key 粒度打散到集群所有 rank 上。对上层 vLLM 的 KV Block 而言，同一轮对话的 blocks 形成**严格依赖链**（每个 block 依赖前一个），一旦链路中任意一个 block 因所在 rank 故障而丢失，后续整条链全部作废。随机打散 + 大 scale（如 48 ranks）导致**单节点故障扩散率极高**：任何一台机器故障即意味着集群内同时所有请求命中率归零。

本方案新增 **节点亲和分配策略** **`ALLOC_LOCAL_FIRST`**，把"按 key 随机打散"升级为"按 backendId（节点）维度分配"：

- **本地优先**：写者实例（P，`role=writer`）分配时，按分布表权重选择目标 backendId，自己（本地）份额高、其他节点份额低，整体仍然均匀——既保留打散效果，又显著缩小故障影响面；
- **写者均匀分流到 memory 节点**：纯内存贡献实例（D，`role=memory`）几乎无写入，通过分布表把各写者的数据按权重均匀引导到 memory 节点（如 3P1D 中 P 侧压力在故障时引导到空闲 D 侧），实现跨实例负载均衡；
- **同 backendId 内随机**：目标 backendId 内的全部 rank 每次分配随机打乱，避免固定顺序导致的扎堆热点。

## 1.2 动机

现状问题：`ALLOC_RANDOM` 按 key 粒度打散到所有 rank，负载均衡但忽略了 KV Block 的依赖链特性——故障扩散率高。之前的 `ALLOC_LOCAL_FIRST` WIP 实现（peer 节点顺序环形顺延）有两个缺陷：

1. **不随机**：节点内 rank 顺序固定（`unordered_set` 迭代序），命中全部压在第一个 rank 上，形成热点；
2. **failover 无引导**：跨节点顺延按 backendId 顺序，无法在部署形态为 3P1D（P 写、D 几乎不写）时主动把写压力引导到空闲的 D 侧。
3. **身份耦合**：Meta 端靠 `preferredRank_[0]` 反推 caller，若调用方显式设置 preferred rank 会破坏本地方向判定。

## 1.3 目标

**目标：**

- 节点亲和：写者数据按权重分配到目标 backendId，本地占大头、其余均匀分流，故障影响面从"整集群"缩小到"目标节点集合"。
- 均匀分流到 memory：写者通过分布表均匀地把部分数据写入 `role=memory` 的节点，喂满 D 侧，同时让每个写者实例的写压力均衡。
- 组内随机打散：目标 backendId 内 ranks 每次分配真随机（`thread_local mt19937`），避免热点扎堆。
- 身份显式化：RPC 显式携带 caller rank，分配逻辑不再依赖 `preferredRank_` 内容。
- fallback 兜底：目标 backendId 分配失败时回退全局随机，保证可用性；并提供计数与日志观测 fallback 频率。
- 向后兼容：不配置新字段时行为与原 `ALLOC_RANDOM` 一致；`role` 缺省回退 writer，存量部署无感。

**非目标：**

- 不做多副本（replicaNum > 1 语义不在本设计范围内扩展）。
- 不做数据搬移/恢复（故障后数据恢复属 HA 能力）。
- 不改变 `put/get/remove` 等公共 API 语义（内部调度能力增强，无 API 破坏性变更）。

# 2. 用例分析（需求分析）

| 用例              | 优先级 | 功能与性能目标                                                    | 备注                                                            |
| --------------- | --- | ---------------------------------------------------------- | ------------------------------------------------------------- |
| 本地优先分配          | P0  | 写者数据按分布表权重落到目标 backendId，本地权重最高，组内 ranks 随机打散              | `ALLOC_LOCAL_FIRST`，`ExpandLocalFirstRanks` 组内 `std::shuffle` |
| 写者均匀分流到非写入节点内存中 | P0  | 3P1D 下 P 侧部分数据按权重写入 D 侧，D 侧被喂满，各写者实例写均衡                    | `BuildBackendDistribution` 贪心账本 + 本地亲和交换                      |
| 分布表自动重建         | P0  | LocalService 注册/注销后按当前 backendId 集合自动重算，无需人工配置             | `RebuildWriteDistributionTable`，持写锁，ptracer 打点                |
| fallback 兜底     | P0  | 目标 backendId 全部 alloc 失败时回退全局随机，写入不失败                      | `InnerAlloc` 内 `ALLOC_RANDOM` 重试，`local_first_fallback` 计数    |
| caller 身份显式化    | P1  | RPC 消息自带 caller rank，不依赖 `preferredRank_` 反推               | `AllocRequest/BatchAllocRequest` 新增 `rankId_` 字段              |
| 可维测             | P2  | fallback 次数入 metric（`/metrics/summary`），分布表构建耗时 ptracer 打点 | `local_first_fallback=N`、`TP_MMC_META_BUILD_DIST_TABLE`       |

# 3. 方案设计

## 3.1 总体方案

分配策略在 MetaService 侧执行，Client 侧无感（沿用既有 `alloc_strategy` flag 上传通道）。整体分三层：

```text
┌─ LocalService 注册时上报 ──────────────────────────────┐
│  BmRegister(rank, mediaType, bm, ..., backendId, role) │
│    backendId = 节点连接 IP/域名（同机多实例共享）          │
│    role      = "writer"(P) / "memory"(D)              │
└──────────────────────────┬─────────────────────────────┘
                           ▼
┌─ MetaService 拓扑与分布表 ─────────────────────────────┐
│  backendIdRoleMap_      (backendId -> role)           │
│  writeDistributionTable_ (写者 backendId → 目标权重表)   │
│  注册/注销 → RebuildWriteDistributionTable() 重算       │
│  读多写少 → shared_mutex 读共享写独占                    │
└──────────────────────────┬─────────────────────────────┘
                           ▼
┌─ 分配执行路径 ─────────────────────────────────────────┐
│  Put/BatchPut → HandleAlloc/HandleBatchAlloc           │
│  → ExpandLocalFirstRanks:                              │
│      SelectTargetBackendForRank(callerRank)  # 权重选   │
│      → GetRanksForBackendId(目标) → shuffle(组内随机)   │
│  → BatchAlloc → InnerAlloc(ALLOC_LOCAL_FIRST)         │
│     → LocalFirstAssign(preferredRank_ 逐个尝试)        │
│     → 全部失败 → ALLOC_RANDOM 兜底 + fallback 计数     │
└────────────────────────────────────────────────────────┘
```

## 3.2 技术选型

**目标选择：**

| 方案                              | 优势                             | 劣势              | 决策     |
| ------------------------------- | ------------------------------ | --------------- | ------ |
| 固定本地 + backendId 顺序环形顺延         | 实现简单，复用原 WIP                   | 不随机、故障无法引导到 D 侧 | 放弃     |
| 权重表（`BuildBackendDistribution`） | 可表达"本地高权重 + 均匀分流 memory"，结构自均衡 | 需在注册/注销时重算      | **选用** |

**分布表构建：** 两阶段贪心——阶段一账本贪心保证每个 backendId 的份额总和守恒、各写者配额相等（`quota = backendId 总数`）；阶段二本地亲和交换让每个写者尽量多写自己，O(n) 遍历即可逼近最优（n = backendId 数）。

**组内随机源：** `thread_local std::mt19937`（每个线程独立真随机种子），避免热路径锁竞争，同时保证"每次 alloc 顺序不同"。`RanksToString` 支持 debug 打印扩展结果。

**身份传递：** `AllocRequest`/`BatchAllocRequest` 新增 `uint32_t rankId_` 字段（client 用本进程 `RankId()` 填充），Meta 端直接用，不再读 `preferredRank_[0]`。

## 3.3 功能与性能设计

### 3.3.1 分布表算法（`BuildBackendDistribution`）

输入：`allBackendIds`（全量）、`writeBackendIds`（writer 角色）、`memoryBackendIds`（memory 角色）。输出：`std::vector<BackendDistribution>`，每个写者 backendId 对应一个权重目标表 `{backendId, weight}[]`。

**阶段一：账本贪心。** 每个 backendId 有 `perBackendShare = writer 数` 的总份额；每个写者有 `quota = backendId 总数` 的配额。写者从**自己的 backendId** 开始消耗（本地优先），不足再按 `allBackendIds` 顺序环形取剩额：

以 2 writer + 1 memory（backendId: b1,w2="P"; b3="D"）为例：

| 写者 | 取数过程                             | 结果                |
| -- | -------------------------------- | ----------------- |
| b1 | 自己配额 3，b1 余 2 → 取 2；b2 余 2 → 取 1 | b1 → {b1:2, b2:1} |
| b2 | 自己起点 b2 余 1 → 取 1；b3 余 2 → 取 2   | b2 → {b2:1, b3:2} |

**阶段二：本地亲和交换。** 遍历每个写者，若权重最大的目标不是本地，则找"持有本地份额最多的其他写者"，按 `min(两份额)` 等量交换，保持各 backendId 消耗总量守恒：

| 步骤 | 动作                                       | 结果                                           |
| -- | ---------------------------------------- | -------------------------------------------- |
| 1  | b1 最大目标 = 本地 b1(2)，跳过                    | b1 → {b1:2, b2:1}                            |
| 2  | b2 最大目标 = b3(2) ≠ 本地；b1 持 b2 份额 1 → 交换 1 | b2 → {b2:2, b3:1}；b1 → {b1:2, b3:1}（b2 归零删除） |

**最终分布**（2P1D 场景）：b1 → `{b1:67%, D:33%}`，b2 → `{b2:67%, D:33%}`——每个写者 **2/3 本地 + 1/3 均匀写入 D**，D 被两个写者平均喂满，写者实例写压力均衡。

### 3.3.2 分配执行路径

核心：`ExpandLocalFirstRanks`（`MmcMetaService` 成员）——按 caller rank 查分布表权重，并根据权重比例随机选出目标 backendId，取该 backendId 全部 ranks 并 `std::shuffle` 后写入 `options.preferredRank_`，交给既有 `LocalFirstAssign` 逐 rank 尝试。

```text
Put/BatchPut（client，本地填充 rankId_）
  │
  ▼
HandleAlloc / HandleBatchAlloc（MetaNetServer）
  └─（options.flags_ & 0xFF == ALLOC_LOCAL_FIRST）
       ▼
ExpandLocalFirstRanks(options, callerRank)
  ├─ SelectTargetBackendForRank(callerRank)      // 加锁查分布表 + 权重随机
  ├─ targetBackendId 为空 → flags_ 改 ALLOC_ARRANGE 兜底
  ├─ GetRanksForBackendId(targetBackendId)        // 目标节点全部 ranks
  └─ std::shuffle(ranks, thread_local mt19937)    // 组内真随机
       └─ options.preferredRank_ = shufffledRanks
       ▼
InnerAlloc（MmcGlobalAllocator，ALLOC_LOCAL_FIRST）
  └─ LocalFirstAssign：遍历 preferredRank_，找到分配器即 Alloc
       ├─ 全部失败 → Free(blobs) → ALLOC_RANDOM 重试
       │     ├─ 计数 local_first_fallback++（metric）
       │     └─ WARN 日志（callerRank + targetBackendId）
       └─ 成功 → 返回 blob
```

### 3.3.3 并发与性能设计

- **分布表读取**：`SelectTargetBackendForRank` 遍历 `writeDistributionTable_`，与 `RebuildWriteDistributionTable`（注册/注销）共用 `rankBackendIdMapLock_`（`std::shared_mutex`），读共享、写独占；分布表在写锁内整体重建（vector 赋值），读取需持锁或拷贝，避免悬垂。
- **组内 shuffle**：`thread_local mt19937` 免锁；每次 alloc O(组内 rank 数)，量级为卡数（个位数），可忽略。
- **分布表重算成本**：O(n²)（n = backendId 数，两阶段遍历），仅注册/注销时触发，低频。
- **批量分配**：`HandleBatchAlloc` 对每个 key 的 options 独立做一次 `ExpandLocalFirstRanks`——每个 key 独立权重随机 + 组内随机，天然打散。如需批量复用分布表可后续优化（每个 key 的分布表一致，仅随机决定不同）。

### 3.3.4 安全隐私与 DFX 设计

**隐私：** 本特性不采集/上报任何用户数据，`backendId`/`role` 为集群拓扑配置，无隐私风险。

**兼容性：**

- `role` 缺省为空串时按 writer 处理（老版本 LocalService 不配置 role 仍可正常参与分配），并打 ERROR 日志提示补齐。
- 目标 backendId 为空（无配置/非写者）时回退 `ALLOC_ARRANGE` / `ALLOC_RANDOM`，不改变原行为。
- `alloc_strategy` 缺省为 random，不配置则行为与原来一致。

**可靠性：**

- caller 身份显式化后，用户显式 preferred rank 与节点亲和判定彻底解耦，不再互相污染。
- fallback 路径清空 `preferredRank_` 后再走 `ALLOC_RANDOM`，避免旧列表干扰。

**可维测：**

- **metric**：`local_first_fallback`（fallback 到全局随机的次数），出现在 `/metrics/summary`，支持 `prometheus` counter `memcache_local_first_fallback_total`。
- **ptracer**：`TP_MMC_META_BUILD_DIST_TABLE` 打点分布表重建耗时。
- **debug 日志**：`ExpandLocalFirstRanks` 打印 caller rank、目标 backendId、扩展后的 preferredRank\_ 列表（`RanksToString`）；`LocalFirstAssign` 打印每次 blob 落点 rank；fallback 打印 callerRank + targetBackendId。

## 3.4 编程与调用设计

### 3.4.1 配置说明

**MetaService 侧无新增配置**（全部由 LocalService 注册上报驱动）。**LocalService 侧**新增两个配置（也支持 Python API 覆盖）：

| 配置项                                | 类型     | 必填 | 默认值    | 含义                                            |
| ---------------------------------- | ------ | -- | ------ | --------------------------------------------- |
| `ock.mmc.local_service.backend_id` | string | 建议 | 空      | 节点连接 IP/域名，同机多个 LocalService 建议共享同一 backendId |
| `ock.mmc.local_service.role`       | string | 可选 | writer | `writer`（P，写者）/ `memory`（D，纯内存贡献）             |

Python API 对应 `LocalConfig.backend_id` / `LocalConfig.role`（默认 `"writer"`），`LocalConfig.alloc_strategy = 8` 开启本策略（`ALLOC_LOCAL_FIRST`）。

### 3.4.2 新增/修改接口一览

| 位置                                  | 接口                                             | 说明                                                   |
| ----------------------------------- | ---------------------------------------------- | ---------------------------------------------------- |
| `common/mmc_locality_distribution`  | `BuildBackendDistribution(all, write, memory)` | 纯函数，两阶段贪心构建写者 → 目标权重表                                |
| `common/mmc_locality_distribution`  | `PickTargetByWeight(dist, rng)`                | 幂等权重随机选目标 backendId                                  |
| `meta_service/mmc_meta_service`     | `RebuildWriteDistributionTable()`              | 注册/注销后重建分布表（ptracer 打点）                              |
| `meta_service/mmc_meta_service`     | `SelectTargetBackendForRank(callerRank)`       | 加锁查表 + 权重选目标；无配置/非写者返回空串                             |
| `meta_service/mmc_meta_service`     | `ExpandLocalFirstRanks(options, callerRank)`   | 节点亲和扩展：目标 backendId 全部 ranks shuffle 后写入优先列表         |
| `proto/mmc_msg_client_meta`         | `AllocRequest/BatchAllocRequest::rankId_`      | 显式 caller rank 字段（client 填充，随 RPC 上送）                |
| `meta_service/mmc_meta_net_server`  | `HandleAlloc/HandleBatchAlloc`                 | 调用 `ExpandLocalFirstRanks` 替换原 `ExpandRanks`         |
| `meta_service/mmc_global_allocator` | `InnerAlloc` ALLOC\_LOCAL\_FIRST 分支            | LocalFirstAssign 失败 → ALLOC\_RANDOM 重试 + fallback 计数 |

### 3.4.3 文件改动清单

**核心实现：**

- `src/memcache/csrc/common/mmc_locality_distribution.h/.cpp`（新增）：分布表构建 + 权重选择纯函数
- `src/memcache/csrc/meta_service/mmc_meta_service.h/.cpp`：`backendIdRoleMap_`、`writeDistributionTable_`、`RebuildWriteDistributionTable`、`SelectTargetBackendForRank`、`ExpandLocalFirstRanks`，锁升级 `shared_mutex`
- `src/memcache/csrc/meta_service/mmc_meta_net_server.cpp`：`HandleAlloc/HandleBatchAlloc` 改用新接口，注释更新
- `src/memcache/csrc/meta_service/mmc_global_allocator.h`：LOCAL\_FIRST fail → RANDOM 兜底 + `local_first_fallback` 计数
- `src/memcache/csrc/meta_service/mmc_locality_strategy.h`：`LocalFirstAssign` 保留（preferredRank\_ 顺序即扩展示图）
- `src/memcache/csrc/proto/mmc_msg_client_meta.h`：alloc 请求加 `rankId_`
- `src/memcache/csrc/client/mmc_client_default.cpp`：构造 alloc 请求填 `rankId_`

**配置/API：**

- `src/memcache/include/mmc.h`、`mmc_def.h`：`backend_id`、`role` 字段
- `src/memcache/csrc/config/mmc_config_const.h`、`mmc_configuration.h`：解析两个新配置
- `src/memcache/csrc/local_service/mmc_local_service_default.cpp`：注册时上报 `backendId`、`role`
- `src/memcache/csrc/mmc.cpp`：默认值、Python API override
- `src/memcache/csrc/python_wrapper/pymmc.cpp`：暴露 `backend_id`、`role`

**DFX：**

- `src/memcache/csrc/meta_service/mmc_meta_metric_manager.*`：`local_first_fallback` 计数
- `src/memcache/csrc/meta_service/mmc_rest_api_facade.cpp`：summary 输出
- `src/memcache/csrc/common/mmc_ptracer.h`：`TP_MMC_META_BUILD_DIST_TABLE`

# 4. 缺点和风险

| 风险                                                                          | 影响                                 | 应对措施                                                                  |
| --------------------------------------------------------------------------- | ---------------------------------- | --------------------------------------------------------------------- |
| 写者本地份额仍依赖分布表按 backendId 几何平均分配，极端数量（n 大而 writer 少）时分摊到 memory 的份额与新写者数不完全线性 | 分流比例与容量不严格成正比                      | 分布表按 `quota = backendId 总数` 几何平均，size 较小（backendId 数）时误差可忽略；后续可改按容量加权 |
| `SelectTargetBackendForRank` 读取分布表需持共享锁                                     | 高频 alloc 路径多一次读写锁开销                | `shared_mutex` 读锁无竞争时开销 \~ns 级；后续可做无锁快照/副本指针（重建时替换指针）                 |
| `role` 配置缺失（空串）                                                             | 旧配置被当 writer（保守回退），memory 节点可能未被识别 | 默认 writer 保证不丢写能力；ERROR 日志提示运维补齐配置                                    |
| 组内 `mt19937` thread\_local 种子依赖 `std::random_device`，极冷启动时随机源初始化慢           | 首线程首次 alloc 微秒级额外开销                | 一次性成本，仅首个线程首次访问触发，后续复用                                                |
| 目标 backendId 全满时 fallback 到全局随机，可能落到"故障节点"                                  | 与 ALLOC\_LOCAL\_FIRST 初衷略有背离       | 全局随机兜底仅保证可用性；故障节点的 allocator 由 BmUnregister 移除，不会命中无效位置               |

# 5. 未解决问题

1. 批量请求（`HandleBatchAlloc`）当前逐 key 独立做权重随机 + 组内随机；是否批量共享分布表后仅做一次随机起点，减少重复遍历，待评估。
2. `quota` 按 backendId 等权是否需升级为"按容量/按 weight 加权"，覆盖 backendId 容量异构场景，待评估。
3. `SelectTargetBackendForRank` 的共享锁读取是否需优化为无锁快照指针，待评估（当前 ns 级开销可接受）。

***

附录

- **参考资料：**
  - 源码：`src/memcache/csrc/common/mmc_locality_distribution.*`、`meta_service/mmc_meta_service.*`、`meta_service/mmc_meta_net_server.cpp`、`meta_service/mmc_global_allocator.h`、`proto/mmc_msg_client_meta.h`
  - 验证脚本：`/home/qzx/develop_repo/verify_local_first.py`
- **术语表：**
  - backendId：节点连接 IP/域名，同机多个 LocalService 共享（节点亲和分组维度）
  - role：`writer`（写者，P）/ `memory`（纯内存贡献，D）
  - 分布表（BackendDistribution）：写者 backendId → 目标 backendId 权重表，注册/注销时重建
  - preferredRank\_：AllocOptions 中的优先 rank 列表；local\_first 下被扩展为目标 backendId 全部 ranks（shuffle 后）
  - local\_first\_fallback：目标 backendId 分配失败后回退到 ALLOC\_RANDOM 的计数
- **文档更新计划：** 同步更新 `docs/zh/memcache_config.md`（`backend_id`、`role` 配置项）。
