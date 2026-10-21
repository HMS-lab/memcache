# Repository Guide

## Build

- **Default Build & Package**: `bash script/build_and_pack_run.sh --build_mode RELEASE --build_test OFF` — generates a `.run` package and a wheel file in the `output/` directory.
- **Debug Build**: `bash script/build_and_pack_run.sh --build_mode DEBUG --incremental` — used for local debugging, reusing the existing `build/` directory.
- Always use `build_and_pack_run.sh` for compilation; do not invoke underlying build scripts directly.

## Testing

- **All C++ Unit Tests (UT)**: `bash script/run_ut.sh` — performs a DEBUG build, executes `output/bin/ut/test_mmc_test`, and generates coverage reports in `output/coverage`.
- **Filter by Name**: `bash script/run_ut.sh meta_service` — the script will pass the `--gtest_filter=*meta_service*` argument.
- Coverage Thresholds (enforced via `script/run_ut.sh`): At least 70% line coverage and 40% branch coverage for the `src/memcache` directory.
- Python Tests: Located in `test/python`. These typically depend on Ascend/NPU, MetaService, or a mock server; do not assume they have passed if the required environment is not set up.

## Pre-commit & Code Style

- Installation: `pip install pre-commit && pre-commit install --install-hooks`.
- Incremental PR Checks: `bash script/ci-pre-commit-pr.sh`.
- Python Rules: Governed by `pre-commit/pyproject.toml` — targets `py310`, 120-character line width, with Pylint and Bandit enabled.
- C/C++ Formatting: Governed by `.clang-format` and `.pre-commit-config.yaml` — uses `clang-format` v18.1.8, 4-space indentation, and a 120-character line width. - For C/C++ naming conventions and rules regarding "magic numbers," refer to `docs/zh/c_cpp_naming.md`.

## Code Standards

- New C/C++ code must adhere to `docs/zh/c_cpp_naming.md`: use `snake_case` for filenames; `UpperCamelCase` for C++ types and functions; `snake_case` for C APIs; `lowerCamelCase` for parameters and local variables; and append an underscore (`_`) to the names of private or protected members.
- Except for conventional values like `0` or `-1`, any number with a distinct semantic meaning must be defined as a named constant or an enumeration value; this also applies to test code.
- New test files should follow the `*_test.cpp` naming convention, and mock implementation files should use `mock_*.cpp`; existing files do not need to be renamed to comply with this.
- Error handling must remain consistent with the project's existing style—including error codes, assertion macros, validation macros, and logging macros—such as `MMC_OK`, `MMC_ERROR`, `MMC_VALIDATE_RETURN`, and `MMC_LOG_ERROR`.
- Security-related requirements are governed by `docs/zh/SECURITYNOTE.md`; this document must be consulted whenever dealing with TLS, certificates, private keys, configuration file permissions, runtime user accounts, or port ranges.

## Runtime and Configuration

- Common configuration files: `config/mmc-meta.conf`, `config/mmc-local.conf`.
- Installation and usage documentation: `docs/zh/install_whl.md`, `docs/zh/install_run.md`.
- Configuration parameter descriptions: `docs/zh/memcache_config.md`.
- Common environment variables: `MMC_META_CONFIG_PATH`, `MMC_LOCAL_CONFIG_PATH`, `MEMCACHE_HYBRID_HOME_PATH`, `LD_LIBRARY_PATH`. See `docs/zh/environment_variables.md` for details.
- During the build process, `PYTHON_HOME` defaults to `/usr/local/python3.11` or `/usr/local`. For unit testing (UT), `ASCEND_HOME_PATH` is set to point to a mock CANN environment.

## Repository Module Boundaries

| Module | Path | Description |
|---|---|---|
| Public C/C++ API | `src/memcache/include/` | Public-facing header files; changes must be reflected in the API documentation. |
| Core Implementation | `src/memcache/csrc/` | Includes client, common, config, entities, local/meta service, net, proto, and under_api. |
| Python Bindings | `src/memcache/python/`, `src/memcache/csrc/python_wrapper/` | Components for the `memcache_hybrid` wheel and `_pymmc`. |
| MetaService Process | `src/memcache/csrc/daemon/` | `mmc_meta_service` |
| Underlying Dependencies | `3rdparty/memfabric_hybrid/` | Dependencies for MemFabric, SMEM, and HYBM. |
| Tests and Examples | `test/`, `example/` | GTest, Python/mock, K8S deployment, and C++/Python examples. |

## External Interfaces and Documentation Synchronization

- Changes to the C API must be synchronized with `docs/zh/memcache_c_api.md`.
- Changes to the C++ API must be synchronized with `docs/zh/memcache_c++_api.md`.
- Changes to the Python API must be synchronized with `docs/zh/memcache_python_api.md`.
- Changes to REST/MetaService behavior must be synchronized with `docs/zh/memcache_restful_api.md`. - For any changes related to installation, configuration, environment variables, or security, please update the corresponding content in `docs/zh/install_whl.md`, `docs/zh/install_run.md`, `docs/zh/memcache_config.md`, and `docs/zh/SECURITYNOTE.md`.

## Artifacts and Dependencies

- Do not commit build artifacts, including: `build/`, `output/`, `src/memcache/python/build/`, `*.egg-info`, wheel files, or shared libraries.
- Key dependencies (as specified in existing build scripts and documentation) include: CMake >= 3.12, a C++17-compliant compiler, Ninja or Make, Python >= 3.8, pybind11, and MemFabric Hybrid.

## License

Mulan PSL v2. New source code files must include copyright and license headers consistent with those in existing files.
