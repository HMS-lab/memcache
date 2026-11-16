# HA 模式下 master 节点频繁切换

## 问题现象

HA 模式下观察到master节点一直在切换，观察master日志发现master在master和backup间频繁切换，初始化时日志中有如下打印：

```ini
2026-06-02 08:57:07.344376 20 warning [MMC_leader_election.cpp:26 Logger] Updated label of self.pod_name=grtest-mmc-1-mmc-meta-0 to labels={'role': 'master'}
2026-06-02 08:57:07.344405 20 debug [MMC_leader_election.cpp:105 ElectionLoop] Election loop circle, pod-grtest-mmc-1-mmc-meta-0, times=33
2026-06-02 08:57:08.832026 24 warning [MMC_leader_election.cpp:189 RenewLease] Renew loop acquired Python GIL, pod-grtest-mmc-1-mmc-meta-0
2026-06-02 08:57:08.848462 24 debug [MMC_leader_election.cpp:206 RenewLease] Succeed in updating lease-grtest-mmc-1-mmc-meta-Lease: curHolder=grtest-mmc-1-mmc-meta-0, leaseDuration=30, renewTime=2026-06-02 08:57:38.848462, current=2026-06-02 08:57:38.848462, retry period=30
2026-06-02 08:57:10.344572 20 debug [MMC_leader_election.cpp:105 ElectionLoop] Election loop circle, pod-grtest-mmc-1-mmc-meta-0, times=64
2026-06-02 08:57:10.344681 20 debug [MMC_leader_election.cpp:107 ElectionLoop] Election loop acquired Python GIL, pod-grtest-mmc-1-mmc-meta-0
2026-06-02 08:57:10.344710 20 debug [MMC_leader_election.cpp:147 CheckLeaderStatus] Pod grtest-mmc-1-mmc-meta-0 labels={'role': 'master'}
2026-06-02 08:57:10.360390 20 warning [MMC_leader_election.cpp:26 Logger] Updated label of self.pod_name=grtest-mmc-1-mmc-meta-0 to labels={'role': 'backup'}
```

## 问题根因

造成问题的原因一般有以下几种：

### 1. 🔥 续约失败 → 降级 → 检查到自己是 holder 又夺回 → 循环

- RenewLoop 每 3s 续约，一旦网络抖动/APIServer 慢/409 冲突导致续约失败，立即 isLeader_=false，label 变 backup
- 同一进程的 ElectionLoop 同样每 3s 检查 lease，发现 holder 还是自己，又立即夺回，label 变 master
- 3s 内 label 就来回跳一次

**解决方案**

检查集群网络配置，或手动修改leaseDurationSeconds 从 10 改大到 20-30。

### 2. 🔥 两节点时钟不同步

- check_leader_status 用本地时间判断租约是否过期
- A 比 B 快 2s → B 认为 lease 已过期 → 抢主
- A 认为 lease 还有效 → 双方冲突
- 部署文档要求手动 set-time，生产环境基本都会偏差

**解决方案**

手动同步POD的时钟

### 3. 🔥 外部控制器自动清理k8s租约信息

绝大部分问题场景都是1、2的情况，如果不是，极有可能是用户的代码定时清理k8s租约信息导致

**解决方案**

需用户自行排查解决
