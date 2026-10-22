# Kubernetes配置示例

本文介绍如何通过两个 Kubernetes 配置文件，在指定节点上发现UBSIO 可以使用的本地块设备、将其注册为 PV，并由 StatefulSet 申请这些 PV 后启动一个 MiniMax 推理 Pod。

> **MetaService 部署形态说明**：本示例中 Meta Service 以单实例随推理 Pod 启动（见 ConfigMap 中的 start_meta.py），使用固定端口，不涉及主备高可用。如需部署 MetaService 主备高可用，请参考《怎么部署一个memcache的HA集群-etcd方案》（etcd 方案，Pod 方式部署见其第 4 章）；注意 etcd 方案下 MetaService 端口由选举动态分配，本示例中固定端口的假设不再适用。请勿使用旧的 K8S Lease 方案。

## 总体架构

整个方案由两个文件协作完成：

| 文件 | 作用 |
| --- | --- |
| `local-static-provisioner.yaml` | 发现节点上的本地块设备，并注册为 Kubernetes PV |
| `statefulset.yaml` | 申请两个本地 PV，并启动 MiniMax 推理实例 |

### 工作流程如下

```text
给节点打 memcache.io/ubsio-node=true 标签
        ↓
该节点启动 Local Static Provisioner DaemonSet
        ↓
在宿主机节点创建好loop盘或者确认可以使用哪些空闲物理盘，创建这些块设备的软链接到/mnt/local-pv/memcache-ubsio目录。
        ↓
Local Static Provisioner 扫描 /mnt/local-pv/memcache-ubsio，为loop盘或者物理盘自动创建 PV。
        ↓
StatefulSet 创建两个 PVC，根据 Pod 所在节点绑定对应的两个本地 PV。
        ↓
PVC 全部 Bound，StatefulSet Pod 和容器启动
```

### 简单理解两个yaml文件的作用

Local Static Provisioner：负责“扫描固定目录的软链接，自动发现并注册已有本地磁盘”。
StatefulSet：负责“申请这些磁盘，并把它们作为裸块设备交给推理容器使用”。

## 使用步骤

下面以“一个目标节点、两个块设备、一个 StatefulSet 副本”为例。两个 PVC 各申请 `900Gi`，因此应准备两个容量均不小于 `900Gi` 的块设备。

### 第一步：在目标节点准备两个块设备

可以直接使用 UBSIO 独占的空闲物理盘，也可以使用 loop 设备。两种方式只选择一种。

参考：[DRAM + SSD 多级池化配置指南](https://gitcode.com/Ascend/memcache/wiki/DRAM%20+%20SSD%20%E5%A4%9A%E7%BA%A7%E6%B1%A0%E5%8C%96%E9%85%8D%E7%BD%AE%E6%8C%87%E5%8D%97.md#%E5%9C%BA%E6%99%AF%E4%B8%80dram--%E7%A9%BA%E9%97%B2%E6%9C%AC%E5%9C%B0ssd%E7%9B%98%E6%8E%A8%E8%8D%90)。loop 设备请查看指南中的“场景二：无本地空闲 SSD 盘”。

### 第二步：给目标节点添加标签

以下命令在可以执行 `kubectl` 的管理节点上运行：

```bash
# 查看节点名称
kubectl get nodes -o wide

# 将 <NODE_NAME> 替换为第一步准备了块设备的节点名称，例如k8s-node1。
kubectl label node <NODE_NAME> memcache.io/ubsio-node=true --overwrite
kubectl label node <NODE_NAME> accelerator=huawei-Ascend910 --overwrite

# 验证标签
kubectl get node <NODE_NAME> \
  -o jsonpath='{.metadata.labels.memcache\.io/ubsio-node}{"\n"}{.metadata.labels.accelerator}{"\n"}'
```

预期输出：

```text
true
huawei-Ascend910
```

`memcache.io/ubsio-node=true` 控制 Local Static Provisioner 是否在该节点运行；两个标签同时存在时，该节点才满足 StatefulSet 的 `nodeSelector`。

### 第三步：创建扫描目录并执行local-static-provisioner.yaml

先在目标节点创建空扫描目录。该操作在应用第一个 YAML 前完成。

```bash
sudo install -d -m 0755 /mnt/local-pv/memcache-ubsio
```

然后在管理节点执行：

```bash
kubectl apply -f local-static-provisioner.yaml

kubectl -n memcache-local-storage rollout status \
  daemonset/local-static-provisioner --timeout=5m

kubectl -n memcache-local-storage get pod -o wide
```

确认 `local-static-provisioner` Pod 已运行在 `<NODE_NAME>`。此时扫描目录还是空的，因此尚未出现 UBSIO Local PV 是正常现象。

### 第四步：把块设备软链接到扫描目录

以下命令在目标节点执行。把 `DEVICE0`、`DEVICE1` 替换为第一步确认的实际设备路径：

```bash
DEVICE0=/dev/loop2
DEVICE1=/dev/loop3
DISCOVERY_DIR=/mnt/local-pv/memcache-ubsio

# 先确认设备存在且确实是块设备
test -b "${DEVICE0}"
test -b "${DEVICE1}"

# 不使用 -f；如果目标名称已存在，命令应直接失败，避免覆盖已有入口。
sudo ln -s "${DEVICE0}" "${DISCOVERY_DIR}/memcache-ubsio0"
sudo ln -s "${DEVICE1}" "${DISCOVERY_DIR}/memcache-ubsio1"
```

如果使用物理盘，只需把变量改为实际设备：

```bash
DEVICE0=/dev/nvme0n1
DEVICE1=/dev/nvme1n1
```

回到管理节点，确认 Provisioner 已创建两个 `Available` Local PV。

```bash
kubectl get pv -l memcache.io/pool=ubsio -o wide
```

如果没有生成两个 PV，检查 Provisioner 日志：

```bash
kubectl -n memcache-local-storage logs \
  daemonset/local-static-provisioner --tail=200
```

### 第五步：填写启动脚本并执行statefulset.yaml

应用 `statefulset.yaml` 前至少完成以下修改和检查：

1. 将 ConfigMap 中的占位 `bootstrap.sh` 和 `start_meta.py` 替换为实际启动逻辑；
2. 实际脚本需要启动推理服务，并在 `8100` 端口提供 `/health`；
3. 如果使用 `8500` 作为 Meta Service 或指标端口，实际进程也必须监听该端口（本条仅适用于单实例部署；若 MetaService 采用 etcd 主备高可用部署，其端口由选举动态分配，`8500` 只能保留作指标端口）；
4. UBSIO 必须使用容器内设备路径例如/dev/memcache-ubsio0:/dev/memcache-ubsio1，不要填写宿主机的 `/dev/loopN` 或 `/dev/nvmeNn1`；
5. 确认推理镜像、Ascend 驱动目录、`/home` 目录和 8 个 NPU 资源在目标节点可用；
6. 若 MetaService 采用 etcd 主备高可用部署，Pod 挂载的 mmc-local.conf 中 `config_store_url` 必须与 MetaService 侧配置相同的 `etcd://` 地址（详见《怎么部署一个memcache的HA集群-etcd方案》）。

UBSIO 磁盘路径的精确配置项名称是 `ubsio.disk.path`。本例应配置为：

```ini
ubsio.disk.path = /dev/memcache-ubsio0:/dev/memcache-ubsio1
```

其中两个路径必须分别与以下 YAML 字段完全一致：

```text
StatefulSet.spec.template.spec.containers[0].volumeDevices[0].devicePath
StatefulSet.spec.template.spec.containers[0].volumeDevices[1].devicePath
```

完成修改后执行：

```bash
kubectl apply -f statefulset.yaml

kubectl -n memcache-inference rollout status \
  statefulset/memcache-minimax --timeout=30m

kubectl -n memcache-inference get statefulset,pod,pvc -o wide
kubectl get pv -l memcache.io/pool=ubsio -o wide
```

预期结果：

- StatefulSet 为 `1/1`；
- Pod `memcache-minimax-0` 为 `Running` 且 Ready；
- 两个 PVC 均为 `Bound`；
- 两个 Local PV 均为 `Bound`；
- 容器内存在两个原始块设备。

验证容器内设备路径和实际容量：

```bash
kubectl -n memcache-inference exec memcache-minimax-0 -- \
  ls -l /dev/memcache-ubsio0 /dev/memcache-ubsio1

kubectl -n memcache-inference exec memcache-minimax-0 -- \
  blockdev --getsize64 /dev/memcache-ubsio0

kubectl -n memcache-inference exec memcache-minimax-0 -- \
  blockdev --getsize64 /dev/memcache-ubsio1
```

### local-pods-demo.yaml配置注意事项

1.pod默认shm只有64M，local-pods-demo.yaml需添加宿主机的shm映射

```text
apiVersion: v1
kind: Pod
metadata:
  name: my-pod
spec:
  containers:
  - name: my-container
    image: nginx
    volumeMounts:
    - name: shm
      mountPath: /dev/shm
  volumes:
  - name: shm
    hostPath:
      path: /dev/shm
      type: Directory
```

2.pod内存配置
memory需大于ubsio.mem.size_in_gb * ubsio.standalone.device_count + vllm活动内存
如：ubsio.mem.size_in_gb=10，ubsio.standalone.device_count=8，vllm活动内存40G，memory至少要大于120G

```text
apiVersion: v1
kind: Pod
metadata:
  name: my-pod
spec:
  containers:
  - name: my-container
    image: nginx
    resources:
        requests:
              memory: "200Gi"
        limits:
              memory: "200Gi"
```

#### 关键参数含义（可根据实际情况修改）

| 参数 | 当前配置值 | YAML 索引位置 | 含义 |
| --- | --- | --- | --- |
| Local Static Provisioner 容器名称 | `provisioner` | `spec.template.spec.containers[0].name` | 运行本地卷发现程序的容器名称，负责扫描节点上的块设备入口并创建 Local PV。 |
| 推理容器名称 | `inference` | `spec.template.spec.containers[0].name` | StatefulSet Pod 中运行推理服务的容器名称。 |
| 推理实例数量 | `1` | `spec.replicas` | 创建的 Pod 副本数。当前会创建一个名为 `memcache-minimax-0` 的 Pod。 |
| Headless Service 名称 | `memcache-minimax` | `Service.metadata.name`；`StatefulSet.spec.serviceName` | 为 StatefulSet 提供稳定网络标识和服务发现。两个位置的名称必须一致；`clusterIP: None` 表示不分配普通 Service 的虚拟 ClusterIP。 |
| StatefulSet 名称 | `memcache-minimax` | `metadata.name` | 管理推理 Pod 及对应 PVC 的有状态工作负载名称。 |
| Local Static 扫描目录 | `/mnt/local-pv/memcache-ubsio` | `ConfigMap.data.storageClassMap` 内嵌 YAML → `memcache-ubsio-local.hostDir`、`memcache-ubsio-local.mountDir`；`DaemonSet.spec.template.spec.containers[0].volumeMounts[2].mountPath`；`DaemonSet.spec.template.spec.volumes[2].hostPath.path` | Provisioner 扫描本地块设备入口的目录。`hostDir` 是宿主机路径，`mountDir` 是 Provisioner 容器内路径；本例二者相同。 |
| 扫描文件名规则 | `memcache-ubsio*` | `data.storageClassMap` 内嵌 YAML → `memcache-ubsio-local.namePattern` | 只处理扫描目录下名称符合该规则的设备入口，例如 `memcache-ubsio0`、`memcache-ubsio1`。 |
| 块设备 StorageClass 名称 | `memcache-ubsio-local` | `StorageClass.metadata.name`；`StatefulSet.spec.volumeClaimTemplates[0].spec.storageClassName`、`volumeClaimTemplates[1].spec.storageClassName` | 将两个 PVC 与 Local Static Provisioner 创建的 PV 关联起来。三个位置的名称必须一致。 |
| 第一个 PVC 模板名称 | `ubsio0` | `spec.volumeClaimTemplates[0].metadata.name` | StatefulSet 为每个 Pod 创建的第一个 PVC 模板。它与 `containers[0].volumeDevices[0].name` 通过名称 `ubsio0` 对应。单副本时 PVC 名称通常为 `ubsio0-memcache-minimax-0`。 |
| 第二个 PVC 模板名称 | `ubsio1` | `spec.volumeClaimTemplates[1].metadata.name` | StatefulSet 为每个 Pod 创建的第二个 PVC 模板。它与 `containers[0].volumeDevices[1].name` 通过名称 `ubsio1` 对应。单副本时 PVC 名称通常为 `ubsio1-memcache-minimax-0`。 |
| 容器内第一个块设备名称 | `/dev/memcache-ubsio0` | `spec.template.spec.containers[0].volumeDevices[0].devicePath` | PVC `ubsio0` 绑定 Block 模式 Local PV 后，通过 `volumeDevices[0]` 映射到容器中的原始块设备路径。 |
| 容器内第二个块设备名称 | `/dev/memcache-ubsio1` | `spec.template.spec.containers[0].volumeDevices[1].devicePath` | PVC `ubsio1` 绑定 Block 模式 Local PV 后，通过 `volumeDevices[1]` 映射到容器中的原始块设备路径。 |
| UBSIO 磁盘路径配置值 | `/dev/memcache-ubsio0:/dev/memcache-ubsio1` | `ubsio.disk.path = /dev/memcache-ubsio0:/dev/memcache-ubsio1` | 多个设备使用英文冒号分隔。这里的两个路径必须与 `StatefulSet.spec.template.spec.containers[0].volumeDevices[0].devicePath` 和 `volumeDevices[1].devicePath` 完全一致。 |
| 容器启动程序 | `/bin/bash` | `spec.template.spec.containers[0].command[0]` | 容器启动时执行的解释器，即 `command` 数组的第一项。 |
| 容器启动脚本 | `/opt/memcache-k8s/bootstrap.sh` | `StatefulSet.spec.template.spec.containers[0].command[1]`；脚本内容位于 `ConfigMap.data.bootstrap.sh` | 推理容器启动后由 Bash 执行的脚本。该文件通过 `memcache-minimax-bootstrap` ConfigMap 挂载到 `/opt/memcache-k8s`。当前为占位脚本，不会启动推理服务。 |
| Meta Service 启动脚本 | `/opt/memcache-k8s/start_meta.py` | 脚本内容位于 `ConfigMap.data.start_meta.py`；挂载目录位于 `StatefulSet.spec.template.spec.containers[0].volumeMounts[2].mountPath` | 文件路径由挂载目录 `/opt/memcache-k8s` 和 ConfigMap 键 `start_meta.py` 组合得到。当前为占位脚本，且不会被 Kubernetes 自动执行；需要由 `bootstrap.sh` 或其他命令主动调用。 |
| 推理容器镜像 | `quay.io/ascend/vllm-ascend:v0.21.0rc1-openeuler` | `spec.template.spec.containers[0].image` | `inference` 容器使用的 vLLM Ascend 镜像。 |
| 推理容器特权模式 | `privileged: false` | `spec.template.spec.containers[0].securityContext.privileged` | 推理容器可以按照非特权模式运行。 |

### local-static-provisioner.yaml

```yaml
# 创建 Local Static Provisioner 使用的命名空间。
apiVersion: v1
kind: Namespace
metadata:
  name: memcache-local-storage
  labels:
    app.kubernetes.io/part-of: memcache-local-pv
---
# Provisioner Pod 访问 Kubernetes API 时使用的身份。
apiVersion: v1
kind: ServiceAccount
metadata:
  name: local-static-provisioner
  namespace: memcache-local-storage
  labels:
    app.kubernetes.io/name: local-static-provisioner
    app.kubernetes.io/version: v2.9.0
---
# 定义设备扫描目录和 Local PV 生成规则。
apiVersion: v1
kind: ConfigMap
metadata:
  name: local-static-provisioner-config
  namespace: memcache-local-storage
  labels:
    app.kubernetes.io/name: local-static-provisioner
    app.kubernetes.io/version: v2.9.0
data:
  # 为 Provisioner 创建的 PV 添加标签，便于按 UBSIO 存储池筛选。
  labelsForPV: |
    memcache.io/pool: ubsio
  storageClassMap: |
    # 必须与下面 StorageClass 的 metadata.name 保持一致。
    memcache-ubsio-local:
      # 【Local Static 扫描目录】宿主机上的块设备发现目录。
      hostDir: /mnt/local-pv/memcache-ubsio
      # 【Local Static 扫描目录】该目录在 Provisioner 容器内的路径。
      mountDir: /mnt/local-pv/memcache-ubsio
      # 生成原始块设备类型的 Local PV，不创建文件系统。
      volumeMode: Block
      # 卷可以被一个节点以读写方式使用。
      accessMode: ReadWriteOnce
      # 只发现名称匹配 memcache-ubsio* 的目录项或设备符号链接。
      namePattern: "memcache-ubsio*"
---
# 定义 Local PV 的绑定和回收行为。
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: memcache-ubsio-local
  labels:
    app.kubernetes.io/part-of: memcache-local-pv
# 不动态创建底层磁盘；底层块设备必须由管理员提前准备。
provisioner: kubernetes.io/no-provisioner
# 等使用 PVC 的 Pod 出现后，再结合节点和调度条件绑定 PV。
volumeBindingMode: WaitForFirstConsumer
# PVC 删除后保留 PV 和底层数据，需要管理员手动回收。
reclaimPolicy: Retain
---
# Provisioner 所需的集群级 RBAC 权限。
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRole
metadata:
  name: memcache-local-static-provisioner-node
rules:
  # 允许发现程序查询、监听、创建和删除 PV 对象。
  - apiGroups: [""]
    resources: ["persistentvolumes"]
    verbs: ["get", "list", "watch", "create", "delete"]
  # 允许读取 StorageClass。
  - apiGroups: ["storage.k8s.io"]
    resources: ["storageclasses"]
    verbs: ["get", "list", "watch"]
  # 允许监听、创建和更新 Kubernetes 事件。
  - apiGroups: [""]
    resources: ["events"]
    verbs: ["watch"]
  - apiGroups: ["", "events.k8s.io"]
    resources: ["events"]
    verbs: ["create", "update", "patch"]
  # 允许读取或更新节点信息，用于识别 PV 所属节点。
  - apiGroups: [""]
    resources: ["nodes"]
    verbs: ["get", "update"]
---
# 将上面的 ClusterRole 授予 Provisioner ServiceAccount。
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRoleBinding
metadata:
  name: memcache-local-static-provisioner-node
subjects:
  - kind: ServiceAccount
    name: local-static-provisioner
    namespace: memcache-local-storage
roleRef:
  apiGroup: rbac.authorization.k8s.io
  kind: ClusterRole
  name: memcache-local-static-provisioner-node
---
# 在每个符合 nodeSelector 的节点上运行一个 Provisioner Pod。
apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: local-static-provisioner
  namespace: memcache-local-storage
  labels:
    app.kubernetes.io/name: local-static-provisioner
    app.kubernetes.io/version: v2.9.0
spec:
  selector:
    matchLabels:
      app.kubernetes.io/name: local-static-provisioner
  updateStrategy:
    type: RollingUpdate
    rollingUpdate:
      # 更新期间最多允许一个目标节点上的 Provisioner Pod 不可用。
      maxUnavailable: 1
  template:
    metadata:
      labels:
        app.kubernetes.io/name: local-static-provisioner
        app.kubernetes.io/version: v2.9.0
    spec:
      # 使用前面创建并授权的 ServiceAccount。
      serviceAccountName: local-static-provisioner
      # 将该存储组件设置为节点关键组件，调度优先级较高。
      priorityClassName: system-node-critical
      nodeSelector:
        kubernetes.io/os: linux
        # 只在显式标记为 UBSIO 节点的机器上运行。
        memcache.io/ubsio-node: "true"
      containers:
        # 【容器名称】Local Static Provisioner 容器名称。
        - name: provisioner
          # 【容器镜像】用于扫描设备并创建 Local PV 的镜像。
          image: k8s.m.daocloud.io/sig-storage/local-volume-provisioner:v2.9.0
          imagePullPolicy: IfNotPresent
          securityContext:
            # 【特权模式】需要访问宿主机 /dev 和本地块设备。
            privileged: true
          env:
            # 通过 Downward API 获取当前 Pod 所在节点名称。
            - name: MY_NODE_NAME
              valueFrom:
                fieldRef:
                  fieldPath: spec.nodeName
            # 通过 Downward API 获取当前 Namespace。
            - name: MY_NAMESPACE
              valueFrom:
                fieldRef:
                  fieldPath: metadata.namespace
            # Provisioner 清理任务使用的容器镜像。
            - name: JOB_CONTAINER_IMAGE
              value: k8s.m.daocloud.io/sig-storage/local-volume-provisioner:v2.9.0
          ports:
            # Provisioner 的指标和健康检查端口。
            - name: metrics
              containerPort: 8080
          livenessProbe:
            tcpSocket:
              port: metrics
            initialDelaySeconds: 10
            periodSeconds: 60
            timeoutSeconds: 5
            failureThreshold: 3
          resources:
            requests:
              cpu: 10m
              memory: 32Mi
            limits:
              cpu: 500m
              memory: 256Mi
          volumeMounts:
            # 将扫描规则 ConfigMap 挂载到 Provisioner 默认配置目录。
            - name: provisioner-config
              mountPath: /etc/provisioner/config
              readOnly: true
            # 访问宿主机块设备。
            - name: provisioner-dev
              mountPath: /dev
            # 【Local Static 扫描目录】容器内扫描路径。
            - name: discovery
              mountPath: /mnt/local-pv/memcache-ubsio
              mountPropagation: HostToContainer
      volumes:
        - name: provisioner-config
          configMap:
            name: local-static-provisioner-config
        - name: provisioner-dev
          hostPath:
            path: /dev
            type: Directory
        - name: discovery
          hostPath:
            # 【Local Static 扫描目录】宿主机目录必须提前存在。
            path: /mnt/local-pv/memcache-ubsio
            type: Directory
```

### statefulset.yaml

```yaml
# 创建推理业务使用的命名空间。
apiVersion: v1
kind: Namespace
metadata:
  name: memcache-inference
  labels:
    app.kubernetes.io/part-of: memcache-minimax
---
# 创建 StatefulSet 配套的 Headless Service。
apiVersion: v1
kind: Service
metadata:
  # 【服务名称】必须与 StatefulSet.spec.serviceName 保持一致。
  name: memcache-minimax
  namespace: memcache-inference
spec:
  # Headless Service 不分配虚拟 ClusterIP，主要提供稳定 DNS 标识。
  clusterIP: None
  selector:
    app.kubernetes.io/name: memcache-minimax
  ports:
    # 【开放端口】vLLM 推理 API 服务端口。
    - name: vllm
      port: 8100
      targetPort: 8100
    # 【开放端口】Meta Service 或监控指标端口。
    - name: metrics
      port: 8500
      targetPort: 8500
---
# 保存容器启动脚本。真实启动逻辑和隐私路径已删除。
apiVersion: v1
kind: ConfigMap
metadata:
  name: memcache-minimax-bootstrap
  namespace: memcache-inference
data:
  # 【启动脚本】占位 Python 文件，不启动 Meta Service。
  start_meta.py: |
    print("start_meta.py placeholder")
  # 【启动脚本】容器启动后执行的占位脚本。
  bootstrap.sh: |
    #!/usr/bin/env bash
    set -Eeuo pipefail

    echo "bootstrap placeholder: replace with the real startup script"
    ls -la /home

    # 仅用于避免占位容器立即退出；不会启动推理服务或健康检查端点。
    exec sleep infinity
---
# 创建有状态推理工作负载。
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: memcache-minimax
  namespace: memcache-inference
spec:
  # 【服务名称】关联上面的 Headless Service。
  serviceName: memcache-minimax
  # 【推理容器数量】创建 1 个 Pod；每个 Pod 包含 1 个 inference 容器。
  replicas: 1
  # 多副本时按序号依次创建，并等待前一个 Pod Ready。
  podManagementPolicy: OrderedReady
  selector:
    matchLabels:
      app.kubernetes.io/name: memcache-minimax
  updateStrategy:
    type: RollingUpdate
  template:
    metadata:
      labels:
        app.kubernetes.io/name: memcache-minimax
        app.kubernetes.io/part-of: memcache-minimax
      annotations:
        # 修改该值可以触发 StatefulSet 重建 Pod，以加载新的 ConfigMap 脚本。
        memcache.io/bootstrap-revision: "4"
    spec:
      # 【端口使用方式】Pod 直接使用节点网络，8100/8500 会占用节点端口。
      hostNetwork: true
      # hostNetwork 场景继续使用集群 DNS。
      dnsPolicy: ClusterFirstWithHostNet
      # 删除 Pod 时最多等待 90 秒，再强制终止容器。
      terminationGracePeriodSeconds: 90
      nodeSelector:
        # Pod 只调度到同时具有 Ascend 910 和 UBSIO 标签的节点。
        accelerator: huawei-Ascend910
        memcache.io/ubsio-node: "true"
      containers:
        # 【容器名称】推理容器名称。
        - name: inference
          # 【容器镜像】推理容器使用的 vLLM Ascend 镜像。
          image: quay.io/ascend/vllm-ascend:v0.21.0rc1-openeuler
          imagePullPolicy: IfNotPresent
          # 【启动脚本】容器启动后由 Bash 执行的脚本路径。
          command: ["/bin/bash", "/opt/memcache-k8s/bootstrap.sh"]
          securityContext:
            # 【非特权模式】以 root 用户运行，但不授予特权模式和 Linux capabilities。
            privileged: false
            allowPrivilegeEscalation: false
            runAsUser: 0
            runAsGroup: 0
            capabilities:
              drop: ["ALL"]
          resources:
            # 申请并限制使用 8 个 Ascend 910 设备。
            limits:
              huawei.com/Ascend910: "8"
            requests:
              huawei.com/Ascend910: "8"
          env:
            # Python 日志不经过缓冲，便于及时查看容器日志。
            - name: PYTHONUNBUFFERED
              value: "1"
            # 通过 Downward API 获取 Pod 所在节点 IP。
            - name: NODE_IP
              valueFrom:
                fieldRef:
                  fieldPath: status.hostIP
          ports:
            # 【开放端口】推理 API 容器端口。
            - name: vllm
              containerPort: 8100
            # 【开放端口】监控指标容器端口。
            - name: metrics
              containerPort: 8500
          # 启动探针：最多约等待 30 分钟让推理服务启动。
          # 当前占位脚本不会提供 /health，因此正式部署前必须替换脚本。
          startupProbe:
            httpGet:
              path: /health
              port: 8100
            periodSeconds: 10
            timeoutSeconds: 3
            failureThreshold: 180
          # 就绪探针失败时，Pod 不再接收 Service 流量。
          readinessProbe:
            httpGet:
              path: /health
              port: 8100
            periodSeconds: 10
            timeoutSeconds: 3
            failureThreshold: 3
          # 存活探针连续失败后，Kubelet 会重启容器。
          livenessProbe:
            httpGet:
              path: /health
              port: 8100
            periodSeconds: 30
            timeoutSeconds: 5
            failureThreshold: 3
          # 将两个 Block 模式 PVC 映射为容器内的原始块设备。
          volumeDevices:
            - name: ubsio0
              # 【容器内块设备名称】第一个 UBSIO 块设备路径。
              devicePath: /dev/memcache-ubsio0
            - name: ubsio1
              # 【容器内块设备名称】第二个 UBSIO 块设备路径。
              devicePath: /dev/memcache-ubsio1
          volumeMounts:
            # 示例工作目录挂载；生产环境建议缩小宿主机目录暴露范围。
            - name: home
              mountPath: /home
            # 使用内存型 emptyDir 替换容器默认 /dev/shm。
            - name: shm
              mountPath: /dev/shm
            # ConfigMap 中的占位脚本挂载到此目录。
            - name: bootstrap
              mountPath: /opt/memcache-k8s
              readOnly: true
            # 将宿主机 Ascend 驱动和工具以只读方式映射到容器。
            - name: ascend-driver
              mountPath: /usr/local/Ascend/driver
              readOnly: true
            - name: ascend-dcmi
              mountPath: /usr/local/dcmi
              readOnly: true
            - name: npu-smi
              mountPath: /usr/local/bin/npu-smi
              readOnly: true
            - name: ascend-install-info
              mountPath: /etc/ascend_install.info
              readOnly: true
      volumes:
        # 示例中映射宿主机 /home；按实际需求替换为最小必要目录。
        - name: home
          hostPath:
            path: /home
            type: Directory
        # 创建最大 400Gi 的内存型共享内存目录。
        - name: shm
          emptyDir:
            medium: Memory
            sizeLimit: 400Gi
        # 把 ConfigMap 的键挂载成可执行的只读文件。
        - name: bootstrap
          configMap:
            name: memcache-minimax-bootstrap
            defaultMode: 0555
        - name: ascend-driver
          hostPath:
            path: /usr/local/Ascend/driver
            type: Directory
        - name: ascend-dcmi
          hostPath:
            path: /usr/local/dcmi
            type: Directory
        - name: npu-smi
          hostPath:
            path: /usr/local/bin/npu-smi
            type: File
        - name: ascend-install-info
          hostPath:
            path: /etc/ascend_install.info
            type: File
  # StatefulSet 根据模板为每个 Pod 创建两个 PVC。
  volumeClaimTemplates:
    - metadata:
        name: ubsio0
      spec:
        # 卷可由一个节点以读写方式使用。
        accessModes: ["ReadWriteOnce"]
        # 必须绑定 Block 模式的 PV，供 volumeDevices 使用。
        volumeMode: Block
        # 必须与第一个 YAML 中的 StorageClass 名称一致。
        storageClassName: memcache-ubsio-local
        resources:
          requests:
            # 只匹配容量不小于 900Gi 的 PV，不会自动创建 900Gi 磁盘。
            storage: 900Gi
    - metadata:
        name: ubsio1
      spec:
        accessModes: ["ReadWriteOnce"]
        volumeMode: Block
        storageClassName: memcache-ubsio-local
        resources:
          requests:
            storage: 900Gi
```
