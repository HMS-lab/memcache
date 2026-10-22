# MemCache扩大A3可用内存空间

## 1. 问题背景

HCCS总线上DRAM地址范围是固定的，而部分服务器的DRAM地址空间并未完全覆盖在HCCS总线地址范围内。只有DRAM地址空间与HCCS总线地址范围存在交集的部分，才能被MemCache申请使用。
当前部分局点存在以下问题：

- **A3  2T内存服务器**：DRAM地址空间与HCCS总线地址空间交织大小约为**680GB**，无法充分利用物理内存资源，发挥MemCache方案的最佳效益。
- **A3  1T内存服务器**：DRAM地址空间与HCCS总线地址空间交织大小为**0GB**，MemCache无法申请有效DRAM地址空间，MemCache方案不可用。

[HCCS总线DRAM地址范围](https://gitcode.com/Ascend/memfabric_hybrid/wiki/FAQ.md#2-%E8%B0%83%E7%94%A8halmemcreate%E8%BF%94%E5%9B%9E%E9%94%99%E8%AF%AF%E7%A0%81-6)：

- 0x29580000000-0x34000000000
- 0xa9580000000-0xb4000000000
- 0x129580000000-0x134000000000
- 0x1a9580000000-0x1b4000000000

由于服务器DRAM地址布局与HCCS总线地址范围不匹配，导致大量物理内存无法被MemCache使用。

## 2. 解决方案

通过调整服务器DRAM地址范围，使DRAM地址空间尽可能与HCCS总线地址范围重合，从而扩大MemCache可申请使用的DRAM空间。

## 3 实施步骤

### 3.1 下载BIOS补丁文件

- Atlas-900-A3-SuperPoD 机型
下载 [Atlas 900 A3 SuperPoD 1.0.8.1 版本及补丁文件](https://support.huawei.com/enterprise/zh/atlas-computing/atlas-900-a3-superpod-pid-261207247/software/269547276?idAbsPath=fixnode01|23710424|251366513|22892968|252309113|261207247) 中的 Atlas-900-A3-SuperPoD-BIOS_32.85.zip 并解压
- Atlas 800T A3 机型
下载 [Atlas 800T A3 1.0.5.1 版本及补丁文件](https://support.huawei.com/enterprise/zh/atlas-computing/atlas-800t-a3-pid-261716443/software/269547277?idAbsPath=fixnode01|23710424|251366513|22892968|252309113|261716443) 中的 Atlas-800T-A3-Atlas-800I-A3-BIOS_32.85.zip 并解压

### 3.2 通过iBMC升级BIOS补丁

**步骤1：登录iBMC管理页面**

进入服务器iBMC管理界面。

**步骤2：进入固件升级页面**

选择：固件升级

![image5.png](https://raw.gitcode.com/user-images/assets/7672915/61ef178d-5a41-4243-af57-01e29f1e4830/image5.png 'image5.png')

**步骤3：上传BIOS补丁并执行升级**

操作路径：固件更新 → 上传BIOS补丁文件 → 开始升级

![image 4.png](https://raw.gitcode.com/user-images/assets/7672915/33c0db8e-e8b7-47d2-a727-1bcf9384811e/image_4.png 'image 4.png')

### 3.3 重启服务器

BIOS补丁升级完成后，需要重启服务器，**并将MSD Base Address Adjust选项设为enable**，使新的DRAM地址空间配置生效。

## 4 验证方法

BIOS补丁生效后，通过以下方式验证优化效果。

**方法1：检查DRAM地址空间变化**

执行：

```bash
lsmem
```

查看服务器DRAM地址空间范围，对比BIOS补丁安装前后的地址空间变化，计算DRAM地址空间与HCCS总线地址范围的交织大小。

**方法2：检查MemCache可申请空间**

获取[mem_scan.py](https://gitcode.com/Ascend/memfabric_hybrid/blob/master/src/smem/python/memfabric_hybrid/memfabric_hybrid/mem_scan.py)脚本，扫描MemCache可申请使用的DRAM空间大小。

```bash
python3 mem_scan.py
```

MemCache可用内存空间应明显增加，扫描结果应接近服务器实际物理内存容量。

## 5 验证案例

### 案例一：A3 1T内存服务器

**BIOS补丁安装前**

服务器DRAM地址空间未覆盖HCCS总线地址范围，无法提供MemCache可用空间。

![1.png](https://raw.gitcode.com/user-images/assets/7672915/a92572ef-63d9-40f1-8b6d-43f509a29d48/1.png '1.png')

**BIOS补丁安装后**

调整后的DRAM地址空间完全覆盖于HCCS总线地址范围内。

![2.png](https://raw.gitcode.com/user-images/assets/7672915/d6d3d030-d3c3-466a-8b15-34fd067f0819/2.png '2.png')

**验证结果：**

* DRAM地址空间与HCCS总线地址实现有效交织；
* MemCache具备可申请使用的DRAM空间。

### 案例二：A3 2T内存服务器

**BIOS补丁安装前**

服务器DRAM地址空间与HCCS总线地址交织范围有限。

![3.png](https://raw.gitcode.com/user-images/assets/7672915/78e8fb62-95d0-4016-8baa-9f4a116fadfc/3.png '3.png')

**BIOS补丁安装后**

调整后的DRAM地址空间已全部覆盖于HCCS总线地址范围内。

![4.png](https://raw.gitcode.com/user-images/assets/7672915/76025082-d093-4862-9496-703209ac9a79/4.png '4.png')

使用mem_scan.py脚本扫描MemCache可申请使用的DRAM空间大小接近2TB，说明BIOS补丁有效扩大了MemCache可利用内存空间。。

![5.png](https://raw.gitcode.com/user-images/assets/7672915/7fbd1f92-6b80-4c22-bc0c-6d16c48d3c98/5.png '5.png')

## 6. 总结

通过调整服务器DRAM地址空间布局，使其与HCCS总线地址范围充分重合，可以显著提升MemCache可使用DRAM容量。

验证结果表明：

- A3 1T服务器由原无法使用提升至可支持MemCache申请；
- A3 2T服务器可用空间提升至接近整机物理内存容量。

该方案能够有效解决因DRAM地址布局不匹配导致的内存资源浪费问题，提升MemCache方案收益，其它类似灵衢总线内存地址问题也可采用此方案。
