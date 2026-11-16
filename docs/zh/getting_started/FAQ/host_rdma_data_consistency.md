# HOST_RDMA 跨机传输后出现数据一致性问题

## 问题现象

get获取到的数据和put不相符

- 单机双client测试无问题
- 跨机双client测试有问题

## 定位思路

检查两节点rdma网卡的MTU设置：

```ini
ip link show
14: enp67s0f0np0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 4500 qdisc mq state UP mode DEFAULT group default qlen 1000
    link/ether e8:eb:d3:3a:53:cc brd ff:ff:ff:ff:ff:ff
```

HCOM最新版本HOST_RDAM传输需要rdma网卡MTU大于4096，如果不正确可通过如下命令修改：

```bash
ip link set enp67s0f0np0 mtu 4500
```
