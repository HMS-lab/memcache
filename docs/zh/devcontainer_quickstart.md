# MemCache Hybrid — Remote Development and Running All Examples

## Prerequisites

- **Remote server**: An Ascend NPU server with the NPU driver installed.
- **Local machine**: VS Code with the Remote-SSH extension.
- **Network**: The local machine can SSH to the remote server, and the remote server can access GitCode and Docker images.

---

## 1. Clone the project on the remote server

SSH into the remote server and clone the repository.

```bash
git clone https://gitcode.com/Ascend/memcache.git
cd memcache
```

---

## 2. Connect with VS Code Remote-SSH

You do not need to start VS Code Server manually on the remote host; VS Code handles this automatically.

**Option A — Connect from the command line** (run in a local terminal)

```bash
code --remote ssh-remote+<your-server-address> /path/to/memcache
```

For example:

```bash
code --remote ssh-remote+192.168.1.100 /home/user/memcache
```

**Option B — Use the VS Code UI**

1. Press `F1` and select `Remote-SSH: Connect to Host...`.
2. Enter `ssh user@host` or select a host from `~/.ssh/config`.
3. Enter the server password.
4. After connecting, select `File > Open Folder...` and choose the `memcache` directory.
5. Enter the server password when prompted.

> [!NOTE]
> VS Code automatically installs `vscode-server` on the remote host. The first connection may take several dozen seconds.

---

## 3. Open the project in a Dev Container

1. Make sure Docker is installed on the remote host and that you have root or docker group permissions.
2. In VS Code, press `F1` and select `Dev Containers: Reopen in Container`.
3. Enter the server password.
4. Wait for the image pull, container build, and `postCreateCommand` to finish.

> [!NOTE]
>
> - The first run pulls `quay.nju.edu.cn/ascend/vllm-ascend:v0.20.2rc1-a3` (about 15–20 GB) and may take a while.
> - The official image is `quay.io/ascend/vllm-ascend:v0.22.1rc1-a3`; the configuration uses a mirror in China to speed up downloads.
> - For an A2 environment, use `quay.nju.edu.cn/ascend/vllm-ascend:v0.20.2rc1`.

`postCreateCommand` automatically performs the following steps (`.devcontainer/post_create.sh`):

- Installs Python development dependencies (pre-commit, pytest, pybind11, and others).
- Initializes the `3rdparty/` submodules.
- Checks toolchain versions (gcc, cmake, ninja, and python).
- Runs a CMake smoke configuration test.
- Installs pre-commit hooks.

> [!NOTE]
> When setup is complete, the lower-left corner of VS Code displays `Dev Container: MemCache Dev Container`.
> Select `show logs` in the lower-right corner to view build details and progress in the terminal.

---

## 4. Build

```bash
# First build (Release mode)
bash script/build_and_pack_run.sh --build_mode RELEASE --build_test OFF

# Debug build (reuse the existing build/ directory)
bash script/build_and_pack_run.sh --build_mode DEBUG --incremental
```

Build artifacts:

- **run package**: `output/memcache_hybrid-*.run`
- **wheel package**: `output/memcache/wheel/memcache_hybrid-*.whl`

---

## 5. Run all examples

### Parameters

| Parameter | Default | Description |
|---|---|---|
| *(none)* | — | Run all Python and C++ examples |
| `--python` | — | Run Python examples only |
| `--cpp` | — | Run C++ examples only (automatically builds and installs the run package) |
| `--meta-config` | `config/mmc-meta.conf` | Path to the MetaService configuration file |
| `--local-config` | `config/mmc-local.conf` | Path to the LocalService configuration file |
| `--run-bench` | off | Run performance benchmarks |
| `--example-timeout` | 180s | Timeout in seconds for each test case |
| `--dry-run` | off | Print commands without running them |
| `--verbose` | off | Show complete output for each test case |

### Examples

```bash
# See which examples would run without executing them
bash script/run_all_examples.sh --dry-run

# Run all examples (Python + C++; MetaService is detected or started automatically)
bash script/run_all_examples.sh

# Run Python examples only
bash script/run_all_examples.sh --python

# Run C++ examples only
bash script/run_all_examples.sh --cpp

# Show detailed output while running
bash script/run_all_examples.sh --verbose
```

### What runs

| Category | Condition | Contents |
|---|---|---|
| `example/python/` — basic CRUD | `--python` + NPU | `test_mmc_demo`, `test_mmc_batch`, `test_mmc_start_meta_service_and_simple_test` |
| `example/python/` — layers | As above + torch_npu | `test_mmc_layers`, `test_mmc_layers_batch` |
| `example/cpp/` | `--cpp` + NPU | Build and run `memcache_cpp_test` (automatically verifies put/get/remove) |
| `example/benchmark/` | `--run-bench` + NPU | 4 KB small-data write/read smoke test |

---

## 6. Run examples manually

### Python examples

```bash
# Option 1: Start MetaService first, then run the examples
export MMC_META_CONFIG_PATH=/path/to/mmc-meta.conf
export MMC_LOCAL_CONFIG_PATH=/path/to/mmc-local.conf
python3 -c "from memcache_hybrid import MetaService; MetaService.main()" &

python3 example/python/test_mmc_demo.py
python3 example/python/test_mmc_batch.py

# Option 2: Use a self-contained example (starts MetaService in the process)
python3 example/python/test_mmc_start_meta_service_and_simple_test.py
```

### C++ examples

```bash
# Install the run package first, then source the environment variables
source /usr/local/memcache_hybrid/set_env.sh
source /usr/local/memfabric_hybrid/set_env.sh

cd example/cpp
mkdir -p build && cmake -B build && make -C build
export MMC_META_CONFIG_PATH=/path/to/mmc-meta.conf
export MMC_LOCAL_CONFIG_PATH=/path/to/mmc-local.conf
./build/memcache_cpp_test
```

### Benchmark

```bash
# Start MetaService first
export MMC_META_CONFIG_PATH=/path/to/mmc-meta.conf
bash example/benchmark/bench_start_meta.sh

# Write test
export MMC_LOCAL_CONFIG_PATH=/path/to/mmc-local.conf
bash example/benchmark/bench_start.sh -t write -p 1 -b 16 -s 1048576 -n 100 -d 1 -e memcache -l npu

# Read test
bash example/benchmark/bench_start.sh -t read -p 1 -b 16 -s 1048576 -n 100 -d 1 -e memcache -l npu
```

---

## Frequently asked questions

**Q: Image pulls are too slow or time out.**

`.devcontainer/Dockerfile` is configured to use the `quay.nju.edu.cn` mirror. If you still need a proxy, configure a registry mirror in `/etc/docker/daemon.json` on the remote host.

**Q: `pybind11` cannot be found during the build.**

`postCreateCommand` installs it automatically. If you skipped that step, install it manually:

```bash
pip install pybind11
```

**Q: Can I run this without an NPU?**

No. MemCache depends on the Ascend NPU drivers (`davinci`, `devmm_svm`), and the build will fail without an NPU environment.

**Q: I only want to use Python examples, not C++ examples.**

```bash
bash script/run_all_examples.sh --python
```

**Q: How do I change the MetaService or LocalService configuration?**

Edit `config/mmc-meta.conf` and `config/mmc-local.conf`, or use `--meta-config` / `--local-config` to specify custom paths.
