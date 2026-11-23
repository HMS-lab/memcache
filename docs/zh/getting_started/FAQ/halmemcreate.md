# HalMemCreate Fails (ret: 6)

## Symptoms

Calling HalMemCreate in vmm_segment produces the following log:

```text
Try HalMemCreate ret: 6
```

## Root Causes

1. Insufficient memory (with a 5.10 kernel).

   If a NUMA node is specified, that node lacks DRAM; otherwise, the environment lacks memory.
   The DRAM address range on the HCCS bus is fixed. In some environments, not all DRAM is on the HCCS bus;
   only memory overlapping the HCCS address range can be allocated.

2. A 6.6 kernel is in use.

   The newer 6.6 kernel's `alloc_contig_range` function does not accept `__GFP_NORETRY` and `__GFP_ACCOUNT`; HDK has not yet adapted to this kernel version.
   See the [Linux kernel commit](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=f6037a4a686523dee1967ef7620349822e019ff8).
   openEuler includes this change in 6.6.0-114.0.0. Download the OS kernel-source package and inspect `mm/page_alloc.c` for the change.

## Solutions

Use `lsmem` to find the machine's physical address ranges. Intersect them with the following four ranges to determine available capacity:

```text
0x29580000000-0x34000000000
0xa9580000000-0xb4000000000
0x129580000000-0x134000000000
0x1a9580000000-0x1b4000000000
```

Other processes compete for memory, so actual allocatable capacity is lower. Restart the machine or reduce the configured memory size.

## Check Available System Memory

Use `mfcli` to scan available physical memory:

`mfcli mem scan`

![image.png](https://raw.gitcode.com/user-images/assets/7672916/348c0854-4efb-4803-8222-f6bf19589fa1/image.png)

MemFabric uses 2 MB and 1 GB huge pages. The default scan uses 1 GB pages. To scan available memory for 2 MB pages, run:

`mfcli mem scan -m 2`

## Further Solutions

1. If more memory is available with 2 MB pages than with 1 GB pages, memory is heavily fragmented. The ZonePageSize mechanism further reduces available memory. Restarting the machine is recommended.

2. Stop processes occupying memory, use `echo 3 > /proc/sys/vm/drop_caches` to drop caches, then use the [separate deployment approach](https://ascend-memcache.readthedocs.io/zh-cn/latest/getting_started/MemCache%2BvLLM%2BA3%E7%8B%AC%E7%AB%8B%E9%83%A8%E7%BD%B2%E6%A1%88%E4%BE%8B/) and start memory-pool processes first.

3. Upgrade the BIOS according to the [guide to increasing available memory in A3 HCCS deployments](https://ascend-memcache.readthedocs.io/zh-cn/latest/user_guide/deployment_guide/MemCache%E6%89%A9%E5%A4%A7A3%E5%8F%AF%E7%94%A8%E5%86%85%E5%AD%98%E7%A9%BA%E9%97%B4/) for a permanent fix.
