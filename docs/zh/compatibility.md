# Complete Software and Hardware Compatibility

| MemCache Requirement | A2 over NPU RDMA Network (device_rdma) | A3 over HCCS Network (device_sdma) | A3 over NPU RDMA Network (device_rdma) | Version Link | How to Check |
| -------------------------------- | ----------------------------------- | ------------------------------- | ----------------------------------- | -------- | -------- |
| LingQu L1 Computing Network | Not required | >=1.5.0 (1.5.1 recommended) | Not required | [LingQu Computing Network versions](https://support.huawei.com/enterprise/zh/ascend-computing/lingqu-computing-network-pid-258003841/software) | [Check the current LingQu L1 software version](https://support.huawei.com/enterprise/zh/doc/EDOC1100536709/ee774920) |
| HDK | >=25.5.1 | >=25.5.1 | >=25.5.1 | [HDK versions](https://support.huawei.com/enterprise/zh/undefined/ascend-hdk-pid-252764743/software) | `npu-smi info` |
| HDK firmware | N/A | >= 7.8.0.6.201 (This is normally correct when HDK is installed properly. If the HDK and LingQu switch versions are correct but an SDMA move comparison error occurs, check this version.) | N/A | | `/usr/local/Ascend/driver/tools/upgrade-tool --device_index -1 --component -1 --version` |
| Page size | 4K; confirm with the development team if using 64K | N/A | 4K; confirm with the development team if using 64K | | `getconf PAGESIZE` |
| OS kernel | N/A | >=5.10.0 | N/A | [Compatibility lookup](https://www.hiascend.com/hardware/compatibility) | `uname -a` |
| BIOS | N/A | Typical memory configurations:<br>1 TB (upgrade required)<br>1.5 TB/2 TB (upgrade recommended to enable more capacity)<br>3 TB (no upgrade required) | N/A | [Upgrade reference](https://gitcode.com/Ascend/memcache/wiki/MemCache%E6%89%A9%E5%A4%A7A3%E5%8F%AF%E7%94%A8%E5%86%85%E5%AD%98%E7%A9%BA%E9%97%B4.md) (for reference only; follow the official guide) | `dmidecode -t bios` |
| Cluster size | 16 nodes | Within a supernode | 8 nodes | | |
