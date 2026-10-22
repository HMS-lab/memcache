# Tag拓扑路由与亲和分配功能说明

## 1. 功能概述

在多机多卡场景中，同一HCCS可达域内的NPU可以通过HCCS进行高速数据传输，不同HCCS可达域之间则需要使用Device RDMA等跨域传输方式。

MemCache通过以下配置实现拓扑感知：

- `tag`：标识当前rank所属的相同传输可达域。
- `tag_op_info`：描述不同tag之间可使用的数据传输方式。
- `tag_affinity_enabled`：控制随机分配时是否优先选择同tag的rank。
- `tag_affinity_high_watermark`：可选的tag高水位；达到水位后，随机分配优先选择其他tag。

这些配置分别作用于两个阶段：

| 阶段 | 作用 |
|---|---|
| 数据放置 | MetaService根据写接口、tag亲和和水位线选择目标rank |
| 数据传输 | 目标rank确定后，MemFabric根据源、目的tag选择传输方式 |

例如，可配置同一tag内使用`DEVICE_SDMA`，通过HCCS完成数据传输；不同tag之间使用`DEVICE_RDMA`，完成跨HCCS可达域传输。

## 2. 基本概念

### 2.1 tag

`tag`表示一个相同传输协议可达域。处于同一相同传输可达域的rank应配置相同的tag，不同可达域应配置不同的tag。

例如：

| 可达域 | rank | tag |
|---|---|---|
| 传输协议0可达域0 | rank 0、rank 1 | `TAG_0` |
| 传输协议1可达域1 | rank 2、rank 3 | `TAG_1` |

tag支持字母、数字和下划线，长度不超过30字节。tag为空时，MemFabric使用内部默认tag，以兼容原有单一传输协议场景。

### 2.2 tag_op_info

`tag_op_info`用于描述tag之间可使用的传输方式，单条配置格式为：

```text
tag1:传输方式:tag2
```

多条路由之间使用英文逗号分隔。例如：

```text
TAG_0:DEVICE_SDMA:TAG_0,TAG_0:DEVICE_RDMA:TAG_1,TAG_1:DEVICE_RDMA:TAG_0,TAG_1:DEVICE_SDMA:TAG_1
```

对应关系如下：

| tag组合 | 传输方式 | 说明 |
|---|---|---|
| `TAG_0`与`TAG_0` | `DEVICE_SDMA` | TAG_0域内通过HCCS传输 |
| `TAG_1`与`TAG_1` | `DEVICE_SDMA` | TAG_1域内通过HCCS传输 |
| `TAG_0`与`TAG_1` | `DEVICE_RDMA` | TAG_0与TAG_1之间跨域传输 |

`tag_op_info`最长为255字节。同一个BM内的所有rank应配置一致的完整路由信息。

当前实现查询传输方式时，先查询`tag1:tag2`，未找到时再查询`tag2:tag1`。因此，对称传输场景只配置一个方向即可被反向复用；为便于阅读和运维，也可以像上例一样显式写出两个方向。不要依赖两个方向使用不同传输方式的非对称配置语义。

### 2.3 tag亲和

tag亲和用于控制随机分配阶段的数据放置顺序。当写请求没有通过`preferredLocalServiceIDs`显式指定目标rank时，客户端会把发起写操作的当前rank作为亲和rank发送给MetaService。

进入随机分配后，候选rank的处理顺序为：

1. 先随机打乱满足介质类型要求的候选rank。
2. tag亲和开启且当前rank具有非空tag时，将同tag候选稳定地排在其他tag之前。
3. 依次尝试候选rank；某个rank空间不足时，继续尝试后续候选。

因此，同一tag内具体选择哪个rank仍然是随机的。tag亲和是优先策略，不是强制策略；容量、介质类型、排除列表和rank可用状态仍然决定最终能否分配成功。

### 2.4 写接口与分配策略

普通写和批量写使用不同的初始分配策略，这是使用tag亲和时最需要注意的区别。

| Python接口 | 初始分配策略 | 单副本且未指定目标rank时的行为 |
|---|---|---|
| `put`、`put_from`、`put_from_layers` | 本rank优先（`ArrangeLocality`） | 先尝试当前rank；成功后直接返回 |
| `put_batch`、`batch_put_from`、`batch_put_from_layers` | 随机分配（`RandomAssign`） | 不先固定写当前rank，直接按tag亲和顺序选择候选rank |

#### 普通put

普通`put`系列接口的底层分配标志为0，MetaService按本rank优先策略处理：

1. 客户端把当前rank写入默认亲和rank。
2. MetaService先在当前rank的目标介质上分配。
3. 单副本分配成功后立即返回，不进入`RandomAssign`。
4. 当前rank无法分配，或仍有其他副本需要分配时，剩余分配才进入`RandomAssign`。

因此，连续执行多个普通单副本`put`，只要当前rank仍有足够空间，这些对象仍会优先放在当前rank。此时不能仅根据“结果位于同tag”判断tag亲和是否生效，因为当前rank本身通常就在当前tag中。

#### batch put

`put_batch`和`batch_put_from`系列接口显式使用`ALLOC_RANDOM`。即使批次中只有一个key，也会直接进入`RandomAssign`：

```python
results = store.batch_put_from(
    [key],
    [buffer_ptr],
    [size],
    direct=memcache_hybrid.L2G,
)
```

开启tag亲和且未达到高水位时，该对象优先分配到当前tag中的任一可用rank，但不保证一定是当前rank或同tag中的某个指定rank。

#### 显式指定preferredLocalServiceIDs

只要`ReplicateConfig.preferredLocalServiceIDs`非空，客户端就会清除原分配策略并设置`ALLOC_FORCE_BY_RANK`。该规则对普通写和批量写都适用：

```python
replicate_config.preferredLocalServiceIDs = [1]
```

对于单副本请求，此时完全按指定rank强制分配，不再执行tag亲和排序。对于多副本请求，各显式指定的rank先强制分配；如果仍有未指定的剩余副本，则剩余部分再按随机策略分配，并排除已分配的rank。

也就是说，`batch_put_from`加`preferredLocalServiceIDs`并不等于“在指定rank附近随机分配”。

### 2.5 tag亲和高水位

`tag_affinity_high_watermark`用于避免随机分配长期集中在当前tag。取值范围为`[0, 100]`：

- `0`：默认值，关闭基于水位的主动跨tag分配。
- `1`至`100`：当前tag达到该使用率后，优先选择低于水位的其他tag。

水位统计和判断具有以下特点：

- 只在`RandomAssign`阶段执行，普通单副本`put`在本rank分配成功时不会检查水位。
- 按本次请求的介质类型分别统计，例如HBM请求只统计各tag的HBM容量和使用量。
- 一个tag的容量和使用量是该tag下所有相关rank的聚合值，不是要求每个rank分别达到水位。
- 当前tag达到水位后，低于水位的其他tag优先；随后是其他非空tag，当前tag排在后面。

## 3. 整体工作流程

以rank 0、rank 1属于`TAG_0`，rank 2属于`TAG_1`为例。

### 3.1 服务启动与拓扑注册

1. LocalService启动时，将本rank的`tag`和`tag_op_info`传给MemFabric创建BM。
2. LocalService向MetaService注册rank时，同时上报该rank的tag。
3. MetaService保存rank与tag的对应关系。

### 3.2 普通单副本put

1. rank 0调用`put`或`put_from`，且不指定`preferredLocalServiceIDs`。
2. 客户端将rank 0作为默认亲和rank发送给MetaService。
3. MetaService首先尝试在rank 0上分配目标介质空间。
4. 如果rank 0分配成功，请求结束；tag随机排序和高水位均不参与本次放置。
5. 如果rank 0无法分配，请求进入随机候选阶段；未达到水位时优先遍历`TAG_0`中的rank，达到水位时按高水位规则调整tag顺序。

### 3.3 batch put

1. rank 0调用`put_batch`或`batch_put_from`。
2. 请求直接进入随机候选阶段，不先固定尝试rank 0。
3. 未达到水位时，rank 0和rank 1作为同tag候选优先，二者之间随机选择。
4. 如果最终选择rank 1，MemFabric根据`TAG_0`与`TAG_0`的路由使用`DEVICE_SDMA`。
5. 当前tag达到水位且TAG_1低于水位时，rank 2优先；MemFabric根据`TAG_0`与`TAG_1`的路由使用`DEVICE_RDMA`。

tag拓扑路由和tag亲和分别解决两个问题：

| 能力 | 解决的问题 |
|---|---|
| tag亲和与高水位 | 随机分配阶段优先把数据放到哪个rank |
| tag拓扑路由 | 目标rank确定后使用哪种方式传输数据 |

## 4. 配置方法

### 4.1 配置文件

各LocalService在现有配置中设置自己的tag，并配置一致的完整路由表。

域0中的rank：

```ini
ock.mmc.local_service.protocol = device_sdma
ock.mmc.local_service.tag = TAG_0
ock.mmc.local_service.tag_op_info = TAG_0:DEVICE_SDMA:TAG_0,TAG_0:DEVICE_RDMA:TAG_1,TAG_1:DEVICE_RDMA:TAG_0,TAG_1:DEVICE_SDMA:TAG_1
```

域1中的rank：

```ini
ock.mmc.local_service.protocol = device_sdma
ock.mmc.local_service.tag = TAG_1
ock.mmc.local_service.tag_op_info = TAG_0:DEVICE_SDMA:TAG_0,TAG_0:DEVICE_RDMA:TAG_1,TAG_1:DEVICE_RDMA:TAG_0,TAG_1:DEVICE_SDMA:TAG_1
```

MetaService通过以下配置控制tag亲和及高水位：

```ini
ock.mmc.tag_affinity.enabled = true
ock.mmc.tag_affinity.high_watermark = 0
```

`ock.mmc.tag_affinity.enabled`默认为`true`；`ock.mmc.tag_affinity.high_watermark`默认为`0`。

### 4.2 Python接口

MetaService配置：

```python
from memcache_hybrid import MetaConfig

meta_config = MetaConfig()
meta_config.tag_affinity_enabled = True
meta_config.tag_affinity_high_watermark = 0
```

LocalService配置：

```python
from memcache_hybrid import LocalConfig

tag_routes = (
    "TAG_0:DEVICE_SDMA:TAG_0,"
    "TAG_0:DEVICE_RDMA:TAG_1,"
    "TAG_1:DEVICE_RDMA:TAG_0,"
    "TAG_1:DEVICE_SDMA:TAG_1"
)

local_config = LocalConfig()
local_config.protocol = "device_sdma"
local_config.tag = "TAG_0"
local_config.tag_op_info = tag_routes
```

不同可达域只需要修改`local_config.tag`，`tag_op_info`应保持一致。容量、服务地址和HBM大小等其他配置仍需根据实际环境设置。

## 5. 生效条件

要使tag拓扑路由和亲和分配按预期生效，需要满足以下条件：

- 同一传输协议可达域内的rank使用相同tag，不同可达域使用不同tag。
- 同一个BM内所有rank使用一致的`tag_op_info`。
- `tag_op_info`覆盖实际可能发生传输的tag组合。
- LocalService重新创建BM后，新的tag和路由配置才会生效。

如果所有rank的tag均为空，MemFabric会为其使用内部默认tag，并根据`protocol`生成同tag兼容路由，保持原有单一协议行为。

如果配置了多个不同的非空tag，则必须为可能发生的跨tag传输提供对应路由。仅将`tag_op_info`留空不能保证跨tag传输回退到`protocol`。

## 6. tag亲和触发条件

tag亲和在以下条件同时满足时生效：

- MetaService的`tag_affinity_enabled`已开启。
- 分配请求中只有一个亲和rank；未显式指定目标时，客户端默认使用发起写操作的当前rank。
- 当前rank已向MetaService注册非空tag。
- 分配已进入`RandomAssign`。

以下情况不会执行同tag优先排序：

- `tag_affinity_enabled`关闭。
- 当前rank的tag为空或MetaService中没有对应tag。
- 调用方通过`preferredLocalServiceIDs`显式指定目标rank；显式指定的副本按指定rank强制分配，不执行同tag排序。
- 请求不满足单一亲和rank条件。
- 普通单副本`put`已在当前rank成功分配，没有进入随机分配阶段。

关闭tag亲和只影响随机分配时的rank排序，不影响`tag_op_info`配置的传输路由。

## 7. 兼容性

- `tag`默认为空。
- `tag_op_info`默认为空。
- `tag_affinity_enabled`默认为开启。
- `tag_affinity_high_watermark`默认为0。
- tag为空时，MemFabric使用内部默认tag；所有rank均采用该默认tag时，分配顺序不会因tag分组发生变化。
- 未配置显式tag拓扑的原有单一协议部署，继续使用`protocol`生成的同tag兼容路由。

因此，未使用多tag拓扑的现有部署不需要修改配置。配置多个非空tag时，应同时配置跨tag路由。

## 8. 日志与问题定位

将MetaService日志级别设置为`debug`后，随机分配中的tag亲和成功时会输出类似日志：

```text
Affinity allocate, currentRank=0, currentTag=TAG_0, selectedRank=1, selectedTag=TAG_0, sameTag=1, spillToOtherTag=0, tagUsed=4096, tagCapacity=2147483648, highWatermark=80
```

关键字段含义：

| 字段 | 含义 |
|---|---|
| `currentRank` | 本次随机分配使用的亲和rank |
| `currentTag` | 亲和rank所属的tag |
| `selectedRank` | 实际分配到的rank |
| `selectedTag` | 实际分配rank所属的tag |
| `sameTag` | 实际分配rank是否与亲和rank属于同一tag；1表示是，0表示否 |
| `spillToOtherTag` | 当前tag是否达到高水位并启用跨tag优先排序 |
| `tagUsed` | 当前tag在本次请求介质上的聚合使用量，单位为字节 |
| `tagCapacity` | 当前tag在本次请求介质上的聚合容量，单位为字节 |
| `highWatermark` | 配置的tag高水位百分比 |

## 9. 配置与使用注意事项

- tag表示统一传输协议可达关系，应按实际硬件拓扑划分，不能只按业务角色或节点名称划分。
- 同一个rank挂载不同介质时必须使用相同tag，否则MetaService会拒绝挂载。
- tag亲和不能替代容量规划；同tag域容量不足时仍会尝试其他tag。
- 高水位只改变`RandomAssign`中的候选顺序，不会把已经分配的数据迁移到其他tag。
- 单key调用批量写接口仍采用随机分配策略；“batch”描述的是接口和请求形式，不代表多副本。
- `replicaNum`决定副本数，与普通写或批量写的选择无关。
- `DEVICE_RDMA`需要对应网络和通信组件可用，否则跨域传输无法正常完成。
- 修改tag、`tag_op_info`或亲和配置后，应保证相关服务按要求重新启动。

更多配置项说明参见[MemCache配置说明](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/memcache_config.md)和[Python API说明](https://gitcode.com/Ascend/memcache/blob/master/docs/zh/memcache_python_api.md)。

## 10. 源码核对依据

本文关键行为按以下实现核对：

- 普通`put`传入分配标志0：`src/memcache/csrc/mmcache_store.cpp`中的`MmcacheStore::Put`和`MmcacheStore::PutFrom`。
- 批量`put`传入`ALLOC_RANDOM`：同文件中的`MmcacheStore::PutBatch`和`MmcacheStore::BatchPutFrom`。
- 显式preferred rank覆盖原策略并改为`ALLOC_FORCE_BY_RANK`：`src/memcache/csrc/client/mmc_client_default.cpp`中的`PrepareAllocOpt`。
- 本rank优先及失败后进入随机分配：`src/memcache/csrc/meta_service/mmc_locality_strategy.h`中的`ArrangeLocality`。
- 随机打乱、同tag排序、高水位和亲和日志：同文件中的`RandomAssign`及`ShouldSpillToOtherTag`。
- tag亲和和高水位默认值：`src/memcache/csrc/config/mmc_config_const.h`及`src/memcache/csrc/mmc.cpp`。
- tag长度与tag_op_info长度限制：`src/memcache/include/mmc_def.h`及`src/memcache/csrc/config/mmc_configuration.h`。
- tag格式、路由解析和反向路由回退：`memfabric_hybrid/src/hybm/csrc/entity/hybm_entity_tag_info.cpp`及`memfabric_hybrid/src/util/csrc/mf_ipv4_validator.h`。
