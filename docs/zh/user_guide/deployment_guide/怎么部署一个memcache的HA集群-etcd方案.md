# etcd后端MetaService主备高可用验证流程

本说明用于介绍如何在etcd后端下，验证MetaService的主备高可用流程。支持裸机进程部署，也支持在K8S集群中以Pod方式部署MetaService（参见第4章）。在生产环境中，用户可以进行参考配置MetaService的主备高可用。

**原理机制**：基于etcd的租约（Lease）和分布式锁，实现MetaService的选主功能。

* 租约（Lease）：每个MetaService以服务端模式接入etcd后端，Leader节点将自身地址注册到etcd（key带TTL约5秒，Leader自动续约）。Leader故障后租约过期，地址key自动删除，集群感知主节点失效。
* 分布式锁：租约过期后，其余MetaService节点竞争分布式选举锁，抢到锁的节点二次确认无Leader后，将自己注册为新Leader（绑定新端口并写入地址key）。
* 动态发现：LocalService客户端不再使用固定地址连接MetaService，而是周期性（2秒）从etcd读取当前Leader地址并重连，Leader切换后自动跟随新主。

选主与切换由MemFabric的HaConfigStore统一实现，MemCache侧只需将config_store_url改为etcd前缀即可启用，无需修改其他代码。

```text
            ┌──────────────┐   租约注册/续约(TTL 5s)   ┌──────────────┐
            │ MetaService  │ ───────────────────────▶ │              │
            │  (Leader)    │ ◀─────────────────────── │     etcd     │
            └──────────────┘     读取Leader地址(2s)    │   (后端)     │
            ┌──────────────┐                          │              │
            │ MetaService  │ ──── 竞争选举锁(待命) ───▶ │              │
            │  (Standby)   │                          └──────────────┘
            └──────────────┘                                  ▲
            ┌──────────────┐                                  │
            │LocalService ×N│ ──── 读取Leader地址，动态重连 ────┘
            └──────────────┘
```

## 1. 环境准备

测试验证推荐用2个节点，本文档基于2个节点，要求910b或最新的芯片，2个节点能互相访问。另需准备一个etcd服务（可部署在其中一个节点上，测试验证用单实例即可）。

注

```text
修改本文档命令中的目录或者IP为自己的目录或者正确的IP
```

### 1.1 确保2台机器时间一致

同时执行如下命令

```shell
sudo timedatectl set-time "2025-10-17 17:30:00"
```

### 1.2 部署etcd服务

测试验证可使用单实例etcd，生产环境必须部署3节点及以上etcd集群（避免协调中心自身单点）。

```shell
# 安装etcd（版本要求3.5及以上）
yum install -y etcd

# 启动单实例etcd，本例监听 10.0.0.1:12335
nohup etcd --listen-client-urls http://10.0.0.1:12335 \
  --advertise-client-urls http://10.0.0.1:12335 \
  --listen-peer-urls http://10.0.0.1:12380 \
  --initial-advertise-peer-urls http://10.0.0.1:12380 \
  --initial-cluster default=http://10.0.0.1:12380 &
```

查询etcd服务正常

```text
# etcdctl --endpoints=http://10.0.0.1:12335 endpoint health
10.0.0.1:12335 is healthy: successfully committed proposal: took = 2ms
```

生产环境3节点etcd集群的搭建请参考etcd官方文档：<https://etcd.io/docs/v3.5/op-guide/clustering/>

### 1.3 确认etcd客户端动态库

本方案依赖动态库`libetcd_client_v3.so`（MemFabric编译产物，构建时需开启`--build_etcd_backend ON`）。安装MemFabric run包后确认库存在：

```shell
source /usr/local/memfabric_hybrid/set_env.sh
ls /usr/local/memfabric_hybrid/latest/etcd/lib64/libetcd_client_v3.so
```

如果缺失该库，请参考[MemFabric的etcd后端使用与部署指导](https://gitee.com/ascend/memfabric_hybrid)重新构建安装。

## 2. 配置文件、日志目录、软件包准备

在2个节点上，建立下面目录结构

```text
/home/meta/
├── bin
├── config
│   ├── mmc-local.conf
│   └── mmc-meta.conf
├── lib
│   └── libhcom.so
├── logs
│   └── logs
└── memfabric_hybrid-1.0.0_linux_aarch64.run  # 请修改为实际路径和文件名
└── memcache_hybrid-1.0.0_linux_aarch64.run   # 请修改为实际路径和文件名
```

### 2.1 配置文件

先往config目录下拷贝一份配置文件模板（配置文件可以通过安装run包方式获取）

```shell
bash memfabric_hybrid-1.0.0_linux_aarch64.run  # 请修改为实际路径和文件名
bash memcache_hybrid-1.0.0_linux_aarch64.run   # 请修改为实际路径和文件名
cp -rf /usr/local/memcache_hybrid/latest/config/* /home/meta/config
```

修改如下配置

1. **设置etcd后端地址（本方案核心配置）**。将config_store_url从tcp://改为etcd://前缀，指向1.2中部署的etcd服务。主备两个MetaService节点、以及所有LocalService节点的config_store_url必须指向同一个etcd后端

   mmc-meta.conf（MetaService节点上修改）

   ```ini
   ock.mmc.meta_service.config_store_url = etcd://10.0.0.1:12335
   ```

   mmc-local.conf（LocalService节点上修改）

   ```ini
   ock.mmc.local_service.config_store_url = etcd://10.0.0.1:12335
   ```

   注：URL格式为`etcd://<ip>:<port>`，ip为etcd监听地址，port必须写全。多套集群共用同一etcd时，需在URL末尾加`#实例ID`做隔离，如`etcd://10.0.0.1:12335#prod`与`etcd://10.0.0.1:12335#test`。

2. **meta_service_url不再需要配置实际地址**。etcd模式下LocalService从etcd动态发现Leader地址，该项保留默认值即可

   mmc-meta.conf / mmc-local.conf

   ```ini
   ock.mmc.meta_service_url = tcp://127.0.0.1:5000
   ```

3. 设置TLS证书相关配置，如不使用TLS可以关闭开关（关闭TLS可能导致安全风险，不建议生产环境采用）

   mmc-meta.conf

   ```ini
   ock.mmc.tls.enable = false
   ock.mmc.config_store.tls.enable = false
   ```

   mmc-local.conf

   ```ini
   ock.mmc.tls.enable = false
   ock.mmc.config_store.tls.enable = false
   ock.mmc.local_service.hcom.tls.enable = false
   ```

4. 设置数据传输协议。这里以host_rdma为例，需要通过show_gids查询rdma网卡ip修改hcom_url中的ip

   mmc-local.conf

   ```ini
   ock.mmc.local_service.hcom_url = tcp://X.X.X.X:7000
   ```

5. 根据启动local-service数量设置world_size

   mmc-local.conf

   ```ini
   ock.mmc.local_service.world_size = 16
   ```

6. metaservice落盘日志路径推荐设置到有磁盘挂载的目录, 方便直接查看服务日志

   mmc-meta.conf

   ```ini
   ock.mmc.log_path = /home/meta/logs
   ```

### 2.2 依赖库

使用host_rdma数据传输协议时，需要将libhcom.so拷贝至/home/meta/lib下，
请参考[hcom项目](https://atomgit.com/openeuler/ubs-comm/tree/br_dev_container_20260228)获取最新的so文件。

### 2.3 软件包

 部署方案依赖[memcache](https://gitcode.com/Ascend/memcache)、[memfabric](https://gitcode.com/Ascend/memfabric_hybrid)，
请按照官方指导编译构建安装包（MemFabric构建时需开启etcd后端：`--build_etcd_backend ON`），并安装到全部节点。

## 3. 启动高可用服务集群

### 3.1 启动MetaService（主备2个进程）

在2个节点上分别启动MetaService进程，两份配置文件的config_store_url完全相同：

```shell
source /usr/local/Ascend/ascend-toolkit/set_env.sh
source /usr/local/memfabric_hybrid/set_env.sh
source /usr/local/memcache_hybrid/set_env.sh
export MMC_META_CONFIG_PATH=/home/meta/config/mmc-meta.conf
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/meta/lib
mmc_meta_service &
```

启动后观察日志，其中一个节点成为Leader，另一个成为Standby：

```text
# Leader节点日志 (/home/meta/logs/logs/mmc-meta.log)
Attempting to become leader
Became leader after 1 attempts
Registered MetaService address in backend: 10.0.0.1:32145
MetaNetServer binding on 10.0.0.1:32145

# Standby节点日志
became follower of 10.0.0.1:32145
```

注：Leader端口由进程在端口范围（默认9000-65535）内自动选择，无需手工配置。可通过环境变量`MF_CONFIG_STORE_PORT_START`/`MF_CONFIG_STORE_PORT_END`收窄端口范围，请保证防火墙放行。

### 3.2 确认选主结果

通过etcd查询当前Leader（值为Leader节点ip:port，无协议前缀）：

```shell
# 查询当前Leader的config store地址
etcdctl --endpoints=http://10.0.0.1:12335 get /memfabric_hybrid/config_store/meta/leader

# 查询LocalService实际连接的MetaService地址
etcdctl --endpoints=http://10.0.0.1:12335 get /memcache/meta/service_addr

# 一次性列出全部相关键
etcdctl --endpoints=http://10.0.0.1:12335 get /memfabric_hybrid --prefix
etcdctl --endpoints=http://10.0.0.1:12335 get /memcache --prefix
```

### 3.3 启动LocalService客户端

在业务节点上启动LocalService（示例以python方式拉起）：

```shell
source /usr/local/Ascend/ascend-toolkit/set_env.sh
source /usr/local/memfabric_hybrid/set_env.sh
source /usr/local/memcache_hybrid/set_env.sh
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/meta/lib
python3 /usr/local/memcache_hybrid/latest/aarch64-linux/script/ha/test-mmc-meta-ha.py > /home/meta/logs/mmc-local.log 2>&1 &
```

最后确认全部正常启动（local日志无报错、连接MetaService成功）

```text
# tail -f /home/meta/logs/mmc-local.log
```

> [!TIP] 注意
>
> * etcd模式下MetaService的地址和端口是动态的，不再需要配置固定端口，也无需创建K8S的Service/ClusterIP/Lease资源
> * 如果业务仍运行在K8S中，可以用Pod方式部署MetaService，具体参见第4章
> * 多套集群共用同一etcd时，务必使用不同的`#实例ID`，否则数据互串

## 4. 在Kubernetes集群中以Pod方式部署（可选）

本章介绍业务运行在K8S集群中时，如何用Pod方式部署etcd高可用方案的MetaService。etcd模式下选主完全由MetaService进程内的HaConfigStore完成（租约+分布式锁），K8S只负责拉起和维持进程，**不参与选主**，因此无需为选主创建任何额外的K8S资源（如Service、RBAC授权、角色标签等）。

**前置条件**：

* 所有MetaService Pod和LocalService Pod均可访问etcd服务端点
* 客户端可直连Leader的动态端口（端口范围默认9000-65535，见4.3节的端口放行说明）
* 已按第2章完成配置文件准备（config_store_url已改为etcd://地址）、镜像已推送到镜像仓

### 4.1 etcd服务部署位置

etcd是本方案的选主依据，其可用性直接决定高可用能力，可选以下两种方式：

* 集群外独立部署（生产环境推荐）：etcd与MemCache集群故障域解耦，按1.2节搭建3节点及以上etcd集群，K8S节点网络可访问即可
* 集群内部署：以StatefulSet方式在K8S内拉起etcd集群（参考etcd官方helm/manifests方案），注意etcd副本与MetaService Pod不要挤在同一故障域

无论哪种方式，etcd地址都写在各节点的配置文件config_store_url中（第2章已配置），K8S侧无需为etcd创建Service供MetaService使用（Pod直连etcd端点即可）。

### 4.2 创建MetaService主备Pod

样例yaml提供MetaService主备Pod的完整定义，使用时需将镜像、安装包等占位内容替换为实际值。**样例仅作参考，存在安全风险，不能直接用于生产环境，用户可以参考样例进行修改定制**

```yaml
# meta-pods-etcd-demo.yaml：etcd方案MetaService主备Pod定义
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: meta-service-pod
  namespace: ns-memcache
spec:
  serviceName: meta-service
  replicas: 2
  selector:
    matchLabels:
      app: meta-service
  template:
    metadata:
      labels:
        app: meta-service   # 主备状态在etcd中，与Pod标签无关
    spec:
      # 进程不操作K8S资源，无需serviceAccountName
      terminationGracePeriodSeconds: 10
      hostNetwork: true     # 注册进etcd的是Pod IP，hostNetwork下即宿主机IP，客户端直连更简单
      dnsPolicy: ClusterFirstWithHostNet
      topologySpreadConstraints:   # 保证主备两个Pod不落在同一节点
        - maxSkew: 1
          topologyKey: kubernetes.io/hostname
          whenUnsatisfiable: DoNotSchedule
          labelSelector:
            matchLabels:
              app: meta-service
      containers:
        - name: meta-service
          image: 127.0.0.1:5000/memcache:1028   # 修改为实际镜像
          command: [ "/bin/bash", "-c" ]
          args:
            - |
              source /usr/local/Ascend/ascend-toolkit/set_env.sh && \
              bash /home/meta/memfabric_hybrid-1.0.0_linux_aarch64.run && \
              bash /home/meta/memcache_hybrid-1.0.0_linux_aarch64.run && \
              echo "install end." && \
              sleep 20 && \
              exec /usr/local/memcache_hybrid/latest/aarch64-linux/bin/mmc_meta_service
          startupProbe:
            exec:
              command: ["sh", "-c", "curl -fsS --max-time 2 http://${META_POD_IP}:8000/livez"]
            failureThreshold: 30
            periodSeconds: 10
            timeoutSeconds: 3
          livenessProbe:
            exec:
              command: ["sh", "-c", "curl -fsS --max-time 2 http://${META_POD_IP}:8000/livez"]
            periodSeconds: 10
            timeoutSeconds: 3
            failureThreshold: 3
          readinessProbe:
            exec:
              command: ["sh", "-c", "curl -fsS --max-time 2 http://${META_POD_IP}:8000/livez"]
            periodSeconds: 10
            timeoutSeconds: 3
            failureThreshold: 3
          env:
            - name: LD_LIBRARY_PATH
              value: "/usr/local/python3.11/lib:/usr/local/memfabric_hybrid/latest/aarch64-linux/lib64:/usr/local/memcache_hybrid/latest/aarch64-linux/lib64"
            - name: MMC_META_CONFIG_PATH
              value: /home/meta/config/mmc-meta.conf   # 已含etcd://地址，无需改写
            - name: META_POD_IP
              valueFrom:
                fieldRef:
                  fieldPath: status.podIP
          volumeMounts:
            - name: host-home
              mountPath: /home
            - name: ascend-dir
              mountPath: /usr/local/Ascend
            - name: host-ping
              mountPath: /usr/local/bin/ping
            - name: smi
              mountPath: /usr/local/bin/npu-smi
            - name: drive
              mountPath: /usr/local/Ascend/driver
            - name: firmware
              mountPath: /usr/local/Ascend/firmware
            # NPU设备挂载：请根据节点实际NPU数量补全（davinci0~N、davinci_manager等设备文件，type: CharDevice）
      initContainers:
        - name: change-pod-ip
          image: 127.0.0.1:5000/memcache:1028
          command: ["/bin/sh", "-c"]
          args:
            - |
              # etcd方案下config_store_url保持etcd://静态地址不改写，
              # 仅metrics_url需按PodIP改写供探针访问
              sed -i '/^ock.mmc.meta_service.metrics_url/c ock.mmc.meta_service.metrics_url = http://metaServiceUrl:8000' /home/meta/config/mmc-meta.conf && \
              sed -i s/metaServiceUrl/$META_POD_IP/ /home/meta/config/mmc-meta.conf
          env:
            - name: META_POD_IP
              valueFrom:
                fieldRef:
                  fieldPath: status.podIP
          volumeMounts:
            - name: host-home
              mountPath: /home
      volumes:
        - name: host-home
          hostPath:
            path: /home
            type: Directory
        # 其余volumes：请根据节点实际环境补全NPU设备等hostPath挂载
```

LocalService客户端以Pod方式部署时，参考test/k8s_deploy目录下的local-pods-demo.yaml，仅要求其挂载的mmc-local.conf中config_store_url已按第2章改为etcd://地址。

创建并确认：

```shell
kubectl apply -f meta-pods-etcd-demo.yaml
kubectl get pod -n ns-memcache -o wide
```

确认选主结果（与裸机方式一致，以etcd为权威）：

```shell
etcdctl --endpoints=http://10.0.0.1:12335 get /memfabric_hybrid/config_store/meta/leader
etcdctl --endpoints=http://10.0.0.1:12335 get /memcache/meta/service_addr
```

### 4.3 端口放行与网络策略

Leader进程会在端口范围（默认9000-65535）内自动选择两个可用端口（config store端口和MetaService端口）注册进etcd，客户端直连这两个端口。K8S环境需注意：

* 若集群启用了NetworkPolicy，默认不会放行如此大的端口范围，需要为meta-service和local-service的Pod显式放行；或者用环境变量收窄端口范围后按段放行：

```shell
# 在meta-pods-etcd-demo.yaml的env中增加（两个MetaService Pod配置相同范围）
- name: MF_CONFIG_STORE_PORT_START
  value: "9000"
- name: MF_CONFIG_STORE_PORT_END
  value: "9100"
```

* 建议端口范围不小于数十个端口：Leader需要一次拿到两个可用端口，范围过窄或端口被残留进程占用会导致竞选失败
* 多机部署时不同节点可复用同端口号（各自绑定本机IP）；样例采用hostNetwork，两个Pod分布在不同节点（topologySpreadConstraints保证），端口不会冲突

### 4.4 在K8S环境验证主备切换

先确认当前Leader所在的Pod（etcd中的ip:port对应某节点的Pod）：

```shell
etcdctl --endpoints=http://10.0.0.1:12335 get /memfabric_hybrid/config_store/meta/leader
kubectl get pod -n ns-memcache -o wide
```

假设Leader为meta-service-pod-0，删除该Pod观察切换：

```shell
kubectl delete pod meta-service-pod-0 -n ns-memcache
```

基于K8S机制，删除一个Pod后，K8S会自动拉起一个新的Pod，以维持Pod数量的稳定。主备切换本身不依赖K8S：原Leader被杀后其etcd租约约5秒过期，备Pod约4秒内感知并竞选为新Leader（切换时间线同5.3节裸机场景，整体恢复一般在1分钟以内）。观察验证：

```shell
# etcd中Leader已切换为另一节点
etcdctl --endpoints=http://10.0.0.1:12335 get /memfabric_hybrid/config_store/meta/leader

# 新Leader所在Pod的日志出现升主记录
kubectl logs meta-service-pod-1 -n ns-memcache -c meta-service | grep -E "Became leader|MetaNetServer binding"

# 业务自动恢复，put/get功能正常
```

> [!NOTE] 注意
>
> * 探针（/livez）只反映进程存活，不反映主备角色，Standby Pod同样是存活状态属正常现象；主备判断以etcd查询结果和日志为准
> * K8S节点本身故障（断电/断网）时，效果等同于杀Leader进程加Pod重建，切换机制相同

## 5. 查询服务运行状态，并验证选主流程

### 5.1 通过以下命令，查询服务运行状况

查询当前Leader（etcd中的值即权威结果）

```shell
etcdctl --endpoints=http://10.0.0.1:12335 get /memfabric_hybrid/config_store/meta/leader
etcdctl --endpoints=http://10.0.0.1:12335 get /memcache/meta/service_addr
```

查询集群规模和存活节点

```shell
etcdctl --endpoints=http://10.0.0.1:12335 get /memfabric_hybrid/config_store/data/world_size
etcdctl --endpoints=http://10.0.0.1:12335 get /memfabric_hybrid/config_store/data/alive_rank_list
```

注：etcd模式下MetaService HTTP接口（/health、/role等）中的主备角色字段可能显示unknown，**不能**作为主备判断依据，请以etcd查询结果和日志为准。

### 5.2 测试MemCache功能正常

```shell
# 交互式测试，输入put/get/remove 等命令验证功能正常
python interactive_app.py
```

#### 5.3 通过以下命令，验证主备HA功能

假设当前Leader为节点1（10.0.0.1），杀掉Leader进程，观察切换：

```shell
# 在Leader节点上执行
kill -9 <mmc_meta_service进程号>
```

切换时间线（整体恢复一般在1分钟以内，实测约10~35秒）：

* Leader被杀后，其etcd租约约5秒内过期，地址key自动删除；
* Standby节点约4秒内感知Leader失效，竞争选举锁成为新Leader，注册新地址；
* 新Leader等待约5秒恢复窗口，接受LocalService重新注册；
* LocalService以2秒周期重新发现Leader地址并自动重连，期间put/get请求会失败重试，重连成功后业务恢复。

验证切换结果

```shell
# 确认Leader已切换为节点2（10.0.0.2）
etcdctl --endpoints=http://10.0.0.1:12335 get /memfabric_hybrid/config_store/meta/leader

# 节点2日志出现升主记录
Attempting to become leader
Became leader after N attempts
MetaNetServer binding on 10.0.0.2:xxxxx

# 业务自动恢复，put/get功能正常
```

> [!TIP] 注意
> 本方案同时覆盖Leader假死场景（进程被SIGSTOP、死锁但TCP未断开）：备节点周期性比对etcd中的Leader记录，不一致时触发Leader降级和重新选举，无需人工干预。

## 6. FAQ

### 6.1 MetaService启动失败，报Failed to open library [libetcd_client_v3.so]

etcd客户端动态库未找到。请确认MemFabric构建时开启了etcd后端（`--build_etcd_backend ON`），并已source环境变量或手动将库路径加入LD_LIBRARY_PATH。

### 6.2 MetaService反复竞选但不成功

检查端口范围（默认9000-65535）内可用端口是否充足。Leader需要绑定两个可用端口（config store端口和MetaService端口），端口范围配置过窄或被残留进程占用会导致竞选失败。可通过环境变量调整：

```shell
export MF_CONFIG_STORE_PORT_START=9000
export MF_CONFIG_STORE_PORT_END=10000
```

### 6.3 常见问题排查方法

查看MetaService日志

```shell
tail -f /home/meta/logs/logs/mmc-meta.log
```

查看LocalService日志

```shell
tail -f /home/meta/logs/mmc-local.log
```

关键字参考：`[ETCD] Connection initialized successfully`（连上etcd）、`Became leader`（升主）、`became follower`（降备）、`MetaNetServer binding on`（服务端口）。

### 6.4 LocalService连接不上MetaService

依次确认：

* etcd服务正常（endpoint health）；
* 两侧config_store_url完全一致（包括#实例ID）；
* LocalService节点网络可达Leader动态端口（etcd中查询service_addr后telnet验证）；
* world_size与实际LocalService数量一致。

**建议**：<br>启动服务时，将服务日志写入有磁盘挂载的目录，便于观察业务运行状况，或定位问题。

## 7. 注意事项

1. 生产环境必须部署3节点及以上etcd集群。进程与etcd之间的连接目前不支持用户名密码认证和TLS（config store、MetaNet、HCOM等其他链路的TLS配置不受影响，仍正常生效），请通过网络隔离（独立etcd集群或安全组白名单）保障etcd访问安全
2. 主备切换窗口内（一般1分钟以内）put/get请求会失败重试，客户端重试预算需覆盖该窗口；自定义客户端请自查重试上限
3. Standby升主后元数据从备份记录和LocalService重新注册恢复，Leader被杀时未落盘的异步备份数据可能丢失，请根据业务敏感度评估asyncFlushIntervalMs配置
4. 多套集群共用同一etcd时，必须通过URL中的#实例ID做隔离
5. 建议参考etcd官方文档对etcd集群进行安全加固
