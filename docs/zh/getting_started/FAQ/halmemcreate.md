# HalMemCreate 失败（ret: 6）

## 问题现象

vmm_segment中调用HalMemCreate，日志中有如下打印：

```text
Try HalMemCreate ret: 6
```

## 问题根因

1. 内存不足（5.10内核情况下）

   如果指定了numa就是当前numa上DRAM不足，如果未指定numa则是当前环境内存不足。
   因为hccs总线上的DRAM地址范围是固定的，部分环境上并不是所有DRAM都在hccs总线上，
   只有和hccs地址有交集的部分可以被申请

2. 使用了6.6版本内核

   6.6的新版本内核的alloc_contig_range函数不支持传入__GFP_NORETRY和__GFP_ACCOUNT，HDK暂未适配该内核版本。
   该修改见 [Linux 内核提交](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=f6037a4a686523dee1967ef7620349822e019ff8)。
   openEuler在6.6.0-114.0.0合入该修改，可以下载当前os的kernel-source包，并查看mm/page_alloc.c文件是否有该pr的修改。

## 解决方法

可以通过lsmem命令查询本机的物理地址范围，和如下4个区间取交集得到具体可用容量：

```text
0x29580000000-0x34000000000
0xa9580000000-0xb4000000000
0x129580000000-0x134000000000
0x1a9580000000-0x1b4000000000
```

另外，因为存在其它进程争抢内存，所以实际可申请量会更低，没有什么解决手段，建议重启或者减少内存配置。

## 查看系统可用内存

运行`mfcli`工具扫描可用的物理内存：

`mfcli mem scan`

![image.png](https://raw.gitcode.com/user-images/assets/7672916/348c0854-4efb-4803-8222-f6bf19589fa1/image.png)

memfabric使用2MB和1GB的大页，脚本默认按照1GB规格扫描，如果要扫描2MB规格大页的可用内存，可以按如下指令运行：

`mfcli mem scan -m 2`

## 进一步解决办法

1. 如果扫描2MB页可用内存比1GB页多，那么就是内存碎片比较严重，这个时候又由于ZonePageSize的机制，导致可用内存急剧减少，没有比较好的方法，推荐重启机器。

2. 推荐杀掉占用进程，并使用 `echo 3 > /proc/sys/vm/drop_caches` 卸载缓存，然后采用[分离部署方案](https://gitcode.com/Ascend/memcache/wiki/MemCache+vLLM+A3分离部署案例.md)的方式，优先拉起内存池进程。

3. 参考[A3 HCCS场景扩大可用内存空间指导](https://gitcode.com/Ascend/memcache/wiki/MemCache扩大A3可用内存空间.md)升级BIOS彻底解决。
