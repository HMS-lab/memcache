# Data Inconsistency After Cross-node HOST_RDMA Transfers

## Symptoms

Data returned by get differs from the data written by put:

- Two clients on one machine work correctly.
- Two clients on different machines show the problem.

## Diagnosis

Check the RDMA NIC MTU on both nodes:

```ini
ip link show
14: enp67s0f0np0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 4500 qdisc mq state UP mode DEFAULT group default qlen 1000
    link/ether e8:eb:d3:3a:53:cc brd ff:ff:ff:ff:ff:ff
```

The latest HCOM HOST_RDMA transport requires an RDMA NIC MTU greater than 4096. If necessary, change it with:

```bash
ip link set enp67s0f0np0 mtu 4500
```
