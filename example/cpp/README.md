# C++

## Implementation Overview

This example provides a basic verification of MemCache APIs.

### Building with CANN Dependencies

This example must be built and run in an NPU environment.

First, install the NPU firmware, driver, and CANN package ([environment installation guide](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha002/softwareinst/instg/instg_0000.html)).

The HDK firmware and driver must use a supported version ([community HDK downloads](https://www.hiascend.com/hardware/firmware-drivers/community)).

After installation, configure the CANN environment variables ([step 3: configure environment variables when installing the Toolkit development package](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha002/softwareinst/instg/instg_0008.html)).

Before running the example, build and install the [**memfabric_hybrid run package**](https://gitcode.com/Ascend/memfabric_hybrid/blob/develop/docs/installation.md). The default installation path is `/usr/local/`. Then source `set_env.sh` under the installation directory.

Example memfabric_hybrid installation commands:

```bash
bash memfabric_hybrid-1.0.0_linux_aarch64.run # Replace with the actual package name
bash memcache_hybrid-1.0.0_linux_aarch64.run  # Replace with the actual package name
source /usr/local/memfabric_hybrid/set_env.sh
source /usr/local/memcache_hybrid/set_env.sh
```

## Build the Example Program

Run the following commands from the current directory (`memcache/example/cpp`):

```bash
mkdir build
cmake . -B build
make -C build
```

## Start the Metadata Service

```shell
export MMC_META_CONFIG_PATH=/usr/local/memcache_hybrid/latest/config/mmc-meta.conf;
export MMC_LOCAL_CONFIG_PATH=/usr/local/memcache_hybrid/latest/config/mmc-local.conf;
mmc_meta_service &
```

## Run

Set up huge pages.

> [!NOTE]
> This is required only for protocols such as device RDMA and host RDMA.

```shell
cat /proc/meminfo
echo 2048 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
```

After building, the executable `build/memcache_cpp_test` is generated in the current directory. Run it as follows to interactively issue commands such as `put` and `get`:

```text
root@localhost:/home/memcache_hybrid/example/cpp# ./build/memcache_cpp_test
```

### Run `put`, `get`, and `remove` Manually

```text
put  1
1
PutFrom result: 0, key: , replicas: 1
put k 1
PutFrom result: 0, key: k, replicas: 1
get k
Checking existence for key: k
GetInto result: 0
Retrieved data hash for key k: 18356023244072079545
remove k
Remove result: 0 for key: k
exit
Exiting program.

```
