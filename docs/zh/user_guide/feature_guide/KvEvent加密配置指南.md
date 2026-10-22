# MemCache KV 事件 CURVE 加密配置指南

三步完成：**生成密钥对 → 配置 Meta 服务（server）→ 配置订阅端（client）**。

---

## 第 1 步：生成密钥对

```bash
cd /path/to/memcache
python ./test/python/memcache/dev_scripts/kv_event_generate_keys.py
```

输出两把钥匙（各 40 字符，Z85 编码）：

```text
public_key: <PUBLIC_KEY>
secret_key: <SECRET_KEY>
```

> 每次运行生成不同的密钥对。**`public_key` 和 `secret_key` 必须来自同一次运行**，不能混搭，否则握手失败且难以排查。

### 验证密钥是否正确（推荐）

编辑 `kv_event_verify_key_pair.py`，把顶部的 `PUBLIC_KEY` / `SECRET_KEY` 填成刚生成的值：

```python
PUBLIC_KEY = "<PUBLIC_KEY>"
SECRET_KEY = "<SECRET_KEY>"
```

运行：

```bash
python ./test/python/memcache/dev_scripts/kv_event_verify_key_pair.py
```

**看到两次 `result: OK`（Test 1 明文 + Test 2 CURVE）即密钥有效**，可以继续。若 Test 2 收不到消息，说明公钥/私钥不是一对，重新生成。

---

## 第 2 步：Meta 服务（server 端）配置

### 方式 A：改配置文件（C++ 跑 meta service 时）

编辑配置文件（如 `config/mmc-meta.conf`），增加：

```ini
# 打开 KV 事件广播
ock.mmc.kv_events.enable = true
# 传输方式：0 = ZMQ（当前唯一支持）
ock.mmc.kv_events.transport = 0
# ZMQ 绑定地址（订阅端连接此地址）
ock.mmc.kv_events.endpoint = tcp://0.0.0.0:5557

# 打开 CURVE 加密
ock.mmc.zmq.enable_curve = true
# server 自己的公钥
ock.mmc.zmq.public_key = <PUBLIC_KEY>
# server 自己的私钥（请勿泄露）
ock.mmc.zmq.private_key = <SECRET_KEY>
```

> `secret_key` 仅保存在 server 端；**`public_key` 需通过安全渠道分发给所有订阅者**。

### 方式 B：Python 启动 meta service

参考 `test_mmc_eviction.py`：

```python
config = MetaConfig()
...
config.kv_events_enable = True
config.kv_events_transport_type = 0
config.kv_events_endpoint = "tcp://127.0.0.1:5557"
config.zmq_enable_curve = True
config.zmq_public_key = "<PUBLIC_KEY>"
config.zmq_private_key = "<SECRET_KEY>"
```

启动后 server 日志应出现：

```text
kv_events: ZMQ CURVE encryption enabled (server-only auth)
```

> 若看到 `FALLBACK TO PLAINTEXT`，说明密钥缺失/格式错误/未配对，回到第 1 步。

---

## 第 3 步：订阅端（client）配置

**client 必须配置两样东西，缺一不可：**

1. **server 的 `public_key`**（由 server 分发）；
2. **client 自己的临时密钥对**——每次连接时现场生成即可，**无需分发给 server**。

原因：libzmq 的 client socket 若只设 server 公钥、不带自己的密钥对，CURVE 握手会**静默失败**（无报错、无消息），极难定位。因此 client 每次连接前调用 `zmq.curve_keypair()` 生成临时密钥对并配置到 socket。

用现成的订阅脚本验证：

```bash
vim ./test/python/memcache/dev_scripts/kv_event_sub.py
```

将 `SERVER_PUB_KEY` 改为 server 的公钥：

```python
SERVER_PUB_KEY = "<PUBLIC_KEY>"
```

该脚本内部已包含 `zmq.curve_keypair()`（每次连接临时生成 client 密钥对），无需手动配置。

运行订阅（endpoint / topic 按 server 配置调整）：

```bash
python ./test/python/memcache/dev_scripts/kv_event_sub.py \
    --endpoint tcp://127.0.0.1:5557 --topic "kv"
```

出现 `curve=on` 即正确：

```text
Subscribed to tcp://127.0.0.1:5557 (topic='kv', curve=on)
```

> 若 server 配置了 `model_name`，topic 变为 `kv@<模型名>`，`--topic` 需对应。

---

## 常见问题（FAQ）

| 现象 | 原因 | 解决 |
| --- | --- | --- |
| server 日志 `FALLBACK TO PLAINTEXT` | 密钥缺失 / 格式错误 / 未配对 | 用 verify 脚本确认密钥；重新生成一对，server 与 client 同步更换 |
| client `curve=off` | 公钥未填或格式错误 | 检查 `SERVER_PUB_KEY` 是否为 40 字符 |
| client `curve=on` 但收不到消息 | client 未配置自己的临时密钥对 | 确认脚本中包含 `zmq.curve_keypair()`（libzmq 4.3.5 必需） |
| 收不到消息且无任何报错 | key 与 server 不一致 / topic 不匹配 / 连接时序 | 核对公钥一致；检查 topic；先启动 server 再启动 client |

---

## 密钥归属速记

| 密钥 | 归属 | 说明 |
| --- | --- | --- |
| server `public_key` | server 配置 + 分发给所有 client | client 用于验证 server 身份并完成握手 |
| server `secret_key` | 仅 server 持有 | 参与加密，绝不外泄 |
| client 密钥对（public + secret） | 仅 client 每次连接临时生成 | 用于完成握手，无需分发，连接结束即失效 |

**client 永远不需要 server 的 `secret_key`，server 也不需要知道 client 的任何密钥。**
