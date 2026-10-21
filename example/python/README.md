# Python

## Implementation Overview

This example provides a basic verification of the `memcache_hybrid` Python APIs.

This example must be built and run in an NPU environment.

### Building with CANN Dependencies

First, install the NPU firmware, driver, and CANN package ([environment installation guide](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha002/softwareinst/instg/instg_0000.html)).

The HDK firmware and driver must use version **25.0.RC1** or later ([community HDK downloads](https://www.hiascend.com/hardware/firmware-drivers/community)).

After installation, configure the CANN environment variables ([step 3: configure environment variables when installing the Toolkit development package](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha002/softwareinst/instg/instg_0008.html)).

Before running the example, build and install the [**memfabric_hybrid run package**](https://gitcode.com/Ascend/memfabric_hybrid/blob/develop/docs/installation.md). The default installation path is `/usr/local/`. Then source `set_env.sh` under the installation directory.

Example memfabric_hybrid installation commands:

```bash
bash memfabric_hybrid-1.0.0_linux_aarch64.run # Replace with the actual package name
bash memcache_hybrid-1.0.0_linux_aarch64.run  # Replace with the actual package name
source /usr/local/memfabric_hybrid/set_env.sh
source /usr/local/memcache_hybrid/set_env.sh
```

## Start the Metadata Service

```shell
export MMC_META_CONFIG_PATH=/usr/local/memcache_hybrid/latest/config/mmc-meta.conf;
python3
>>> from memcache_hybrid import MetaService
>>> MetaService.main()

```

You can also configure the startup parameters directly in Python (without `MMC_META_CONFIG_PATH`).

```python
from memcache_hybrid import MetaConfig, MetaService

config = MetaConfig()
config.meta_service_url = "tcp://192.168.1.1:5000"
config.config_store_url = "tcp://192.168.1.2:6000"
config.metrics_url = "http://192.168.1.1:8000"
config.ha_enable = False
config.log_level = "info"

MetaService.setup(config)
MetaService.main()
```

## Configure Huge Pages

> [!NOTE]
> This is required only for protocols such as device RDMA and host RDMA.

```shell
cat /proc/meminfo
echo 2048 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
```

## Run the Script

Choose a script and run it directly. For example, `test_mmc_start_meta_service_and_simple_test.py` starts the MetaService and LocalService in the same process, then runs put/get tests; the MetaService does not need to be started separately. To change parameters, edit the relevant `MetaConfig` and `LocalConfig` code.

```shell
python python/test_mmc_start_meta_service_and_simple_test.py
```
