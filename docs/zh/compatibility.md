# 完整底软配套

| MemCache配套要求                 | A2 over NPU RDMA网络（device_rdma） | A3 over HCCS网络（device_sdma） | A3 over NPU RDMA网络（device_rdma） | 版本链接 | 检查方法 |
| -------------------------------- | ----------------------------------- | ------------------------------- | ----------------------------------- | -------- | -------- |
| 灵衢 L1 LingQu Computing Network | 不依赖                              | >=1.5.0（优选1.5.1）            | 不依赖                              |      [LingQu Computing Network 版本](https://support.huawei.com/enterprise/zh/ascend-computing/lingqu-computing-network-pid-258003841/software)    |    [查询当前灵衢L1软件版本](https://support.huawei.com/enterprise/zh/doc/EDOC1100536709/ee774920)      |
| HDK                              | >=25.5.1                            | >=25.5.1                        | >=25.5.1                            |   [HDK版本](https://support.huawei.com/enterprise/zh/undefined/ascend-hdk-pid-252764743/software)       |   `npu-smi info`       |
| HDK-firmware                     | 不涉及                              |      >= 7.8.0.6.201（一般正常安装HDK此配套是准确的，如果发现hdk版本，灵衢交换机版本都对，但是报sdma move comperr ，可以检查一下此配套）                   |  不涉及                            |          |   `/usr/local/Ascend/driver/tools/upgrade-tool --device_index -1 --component -1 --version`       |
| PageSize                         | 4K，如果是64K，需要开发确认         |        不涉及                 | 4K，如果是64K，需要开发确认         |          |     `getconf PAGESIZE`     |
| OS kernel                        | 不涉及                              |      >=5.10.0               |       不涉及                  |  [兼容性查询](https://www.hiascend.com/hardware/compatibility)        |    `uname -a`      |
| Bios                             | 不涉及                              |  典型内存配置：<br>1TB （必须升级）<br>1.5TB/2TB（建议升级扩展更大空间）<br>3TB（无需升级）      |     不涉及        |   [升级参考](https://gitcode.com/Ascend/memcache/wiki/MemCache%E6%89%A9%E5%A4%A7A3%E5%8F%AF%E7%94%A8%E5%86%85%E5%AD%98%E7%A9%BA%E9%97%B4.md)（仅参考，以指导书为准）   |    `dmidecode -t bios`      |
| 集群规模                         | 16节点                              | 超节点内                        | 8节点                               |          |          |
