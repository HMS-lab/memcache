# Install and Use the Wheel Package

This guide explains how to install and use the wheel package. It is intended for users of the Python API.

## Install the Software

The wheel package is published on [PyPI](https://pypi.org/project/memcache-hybrid/#files) and can be installed directly:

```bash
# Install the latest version
pip install memcache_hybrid
```

```bash
# Check the version and installation path
pip show memcache_hybrid
```

## Use the Software

### Install the MemFabric Dependency

MemCache requires MemFabric at runtime.

```bash
# Check memfabric_hybrid
pip show memfabric_hybrid
```

- MemCache 1.1.x and later automatically installs the compatible MemFabric dependency, so no separate installation is needed.
- For MemCache 1.0.x, see the [release compatibility information](https://gitcode.com/Ascend/memcache/releases) to find the matching MemFabric version.

```bash
# Example for version 1.0.8
pip install memfabric_hybrid==1.0.8
```

See the [MemFabric installation guide](https://gitcode.com/Ascend/memfabric_hybrid/blob/develop/docs/zh/installation.md) for details.

### Run the Software

There are two ways to start the software. They differ in how configuration options are set:

- Option 1: Set configuration options directly through the Python API (recommended).
- Option 2: Set configuration options through configuration files and environment variables (for compatibility with the previous method).

> [!NOTE]
> MemCache 1.0.x supports only option 2. For version 1.1.x and later, choose either option.

#### Option 1: Set Configuration Through the Python API (Recommended)

**1. Start MetaService**

MetaService runs as a standalone process. Start it from a Python console or a Python script:

```python
from memcache_hybrid import MetaService, MetaConfig

config = MetaConfig()
config.meta_service_url = "tcp://127.0.0.1:5000"
config.config_store_url = "tcp://127.0.0.1:6000"
config.metrics_url = "http://127.0.0.1:8000"
config.ha_enable = False
config.log_level = "info"

MetaService.setup(config)
MetaService.main()
```

**2. Start LocalService**

LocalService acts as a client and exposes APIs to application processes through the wheel package. The following example sets configuration through the Python API.

Run this in a Python console or script to start the example process:

```python
from memcache_hybrid import DistributedObjectStore, LocalConfig

config = LocalConfig()
config.protocol = "device_rdma"
config.dram_size = "10GB"
config.max_dram_size = "64GB"
print(config)

store = DistributedObjectStore()
assert store.setup(config) == 0, "setup local config failed"
```

See the [configuration reference](./memcache_config.md) for available options. Set their values to match your environment and workload.

> [!NOTE]
> Configuration set this way takes precedence over configuration from option 2. If both methods are used, the values set through this method are used at runtime.

#### Option 2: Set Configuration Through Files and Environment Variables (Compatibility Method)

**1. Set up the configuration files**

After installing with pip, use the following command to find the installation location:

```bash
# Show the installation location and version
pip show memcache_hybrid
```

Example output:

```text
Name: memcache_hybrid
Version: 1.0.0
Summary: python api for memcache_hybrid
Home-page: https://gitcode.com/Ascend/memcache
Author:
Author-email:
License: Mulan PSL v2
Location: /usr/local/lib/python3.11/site-packages
Requires:
Required-by:
```

If the package is installed at `/usr/local/lib/python3.11/site-packages`, its configuration files are under `/usr/local/lib/python3.11/site-packages/memcache_hybrid/config`.

> [!NOTE]
>
> - MemCache 1.0.x does not include configuration files in the installation directory. Get them from the local [`config/`](../../config/) directory.
> - MemCache 1.2.x and later wheels include UBSIO disk management by default. The `config` directory contains `mmc-meta.conf`, `mmc-local.conf`, and `ubsio.conf`.

The runtime configuration files `mmc-meta.conf` (MetaService settings), `mmc-local.conf` (LocalService settings), and `ubsio.conf` (UBSIO/SSD disk management settings) can be stored in any directory.

**Copy these configuration files elsewhere (for example, to `/usr/local`) before editing them to prevent them from being overwritten during reinstallation.**

Before running, configure `mmc-meta.conf` and `mmc-local.conf` according to the [configuration reference](./memcache_config.md).

**2. Set environment variables**

Use environment variables to specify the configuration file paths.

- The configuration files can be stored anywhere. The following example uses the default installation path; change it to the actual path.
- Wheels include UBSIO disk management by default. To use SSDs, update the configuration and set this environment variable; see the SSD usage guide.

```bash
export UBSIO_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/ubsio.conf
export MMC_META_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-meta.conf
export MMC_LOCAL_CONFIG_PATH=/usr/local/lib/python3.11/site-packages/memcache_hybrid/config/mmc-local.conf
```

**3. Start MetaService**

Start the process from a Python console or script:

```python
from memcache_hybrid import MetaService
MetaService.main()
```

**4. Start LocalService**

Initialize the client through the MemCache [API](./memcache_python_api.md) to start LocalService and perform data writes, queries, reads, and removals.

The script [test_mmc_demo.py](https://gitcode.com/Ascend/memcache/blob/master/example/python/test_mmc_demo.py) is one example:

```bash
python3 test_mmc_demo.py
```

## Uninstall the Software

```bash
pip uninstall memcache_hybrid
```
