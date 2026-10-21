# Build, Install, and Use the Run Package

This guide explains how to install and use the run package. It is intended for users of the C++ API.

No prebuilt run package is published; users must build it themselves.

## Build the Run Package

### Prerequisites

Recommended tool versions:

- OS: Ubuntu 22.04 LTS+
- cmake: 3.20.x
- gcc: 11.4+
- python: 3.11.10
- pybind11: 2.10.3
- make: 4.3 or ninja: 1.10.1

### Build

**1. Clone the source code**

```bash
git clone https://gitcode.com/Ascend/memcache
cd memcache
git clean -xdf
git reset --hard
```

**2. Fetch third-party libraries**

```bash
# Initialize and update only the required submodules (excluding test dependencies)
git submodule update --init 3rdparty/
```

- `--init 3rdparty/` initializes and updates all submodules under `3rdparty/` (including spdlog, nlohmann, msgpack-c, and libzmq), without fetching unnecessary dependencies such as those under `test/`.
- To fetch all submodules, including test dependencies, run `git submodule update --recursive --init`.

**3. Build**

Run the following command:

```bash
bash script/build_and_pack_run.sh --build_mode RELEASE --build_test OFF
```

- `build_and_pack_run.sh` accepts two parameters: `--build_mode <build_mode>` and `--build_test <build_test>`.
- `build_mode`: Build type. Options are `RELEASE`, `DEBUG`, or `ASAN`; the default is `RELEASE`.
- `build_test`: Whether to package test tools. Options are `ON` or `OFF`; the default is `OFF`.

After a successful build, the run package is placed in `output/`, and the wheel package is placed in `output/memcache/wheel/`.

## Install the Run Package

### Install MemFabric

MemCache depends on MemFabric, which must be installed first. See the [MemFabric installation guide](https://gitcode.com/Ascend/memfabric_hybrid/blob/develop/docs/zh/installation.md).

### Install MemCache

All MemCache features are included in the run package, named `memcache_hybrid-${version}_${os}_${arch}.run`.

Here, `version` is the MemCache version, `os` is the operating system (for example, Linux), and `arch` is the architecture (for example, `x86_64` or `aarch64`).

The default installation root is `/usr/local/`.

For example:

```bash
cd output
bash memcache_hybrid-*_linux_aarch64.run # Replace with the actual path and filename
```

To choose a custom installation path, use the `--install-path` parameter:

```bash
bash memcache_hybrid-*_linux_aarch64.run --install-path=${your path} # Replace with the actual path and filename
```

To check the installed version (using the default installation path):

```bash
cat /usr/local/memcache_hybrid/latest/version.info
```

## Use the Software

### Set Environment Variables

```bash
# This uses the default installation paths; change them to the actual paths
source /usr/local/memcache_hybrid/set_env.sh
source /usr/local/memfabric_hybrid/set_env.sh
```

### Start MetaService

MetaService runs as a standalone process. Start it after configuring its settings.

**1. Edit the configuration file**

After installation, the configuration file is at `memcache_hybrid/latest/config/mmc-meta.conf` under the installation directory.

**Copy the configuration file elsewhere before editing it to prevent it from being overwritten during reinstallation.**

Before running, configure `mmc-meta.conf` according to the [MetaService configuration options](./memcache_config.md).

**2. Set the configuration file path**

```bash
# This uses the default installation path; change it to the actual path
export MMC_META_CONFIG_PATH=/usr/local/memcache_hybrid/latest/config/mmc-meta.conf
```

**3. Start MetaService**

```bash
/usr/local/memcache_hybrid/latest/aarch64-linux/bin/mmc_meta_service
```

### Start LocalService

LocalService acts as a client and provides APIs to application processes through a shared library.

**1. Edit the configuration file**

After installation, the configuration file is at `memcache_hybrid/latest/config/mmc-local.conf` under the installation directory.

**Copy the configuration file elsewhere before editing it to prevent it from being overwritten during reinstallation.**

Before running, configure `mmc-local.conf` according to the [LocalService configuration options](./memcache_config.md).

**2. Set the configuration file path**

```bash
# This uses the default installation path; change it to the actual path
export MMC_LOCAL_CONFIG_PATH=/usr/local/memcache_hybrid/latest/config/mmc-local.conf
```

**3. Include the header**

```cpp
#include "mmcache.h"
```

**4. Call the APIs**

Use the MemCache [APIs](./memcache_c++_api.md) to initialize the client and perform data writes, queries, reads, and removals.

See the example source file [memcache_cpp_test.cpp](https://gitcode.com/Ascend/memcache/blob/master/example/cpp/memcache_cpp_test.cpp).

## Uninstall the Software

Run the uninstall script at `memcache_hybrid/latest/uninstall.sh` under the installation directory:

```bash
# This uses the default installation path; change it to the actual path
bash /usr/local/memcache_hybrid/latest/uninstall.sh
```
