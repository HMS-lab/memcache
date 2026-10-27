# 贡献指南

欢迎参与 Memcache！无论是修复代码、补充测试，还是完善文档和分享使用反馈，都能帮助项目改进。
参与项目讨论和贡献时，请遵守[社区行为准则](./CODE_OF_CONDUCT.md)。

## 从哪里开始

你可以通过以下方式贡献：

- 修复缺陷、KVCache数据读写、元数据管理等。
- 补充单元测试、回归测试和异常路径验证。
- 完善 `docs/` 下的文档，修正文档中的命令、路径和链接。
- 分享性能实验和应用集成反馈，并记录环境、复现步骤及测量结果。

先查看 [GitCode Issues](https://gitcode.com/Ascend/memcache/issues)，搜索是否已有相关问题。
首次参与可以关注 `good-first-issue` 标签，并结合自己的环境和经验选择任务。
认领前阅读问题描述、评论和关联 PR，在 Issue 下说明拟开展的工作，避免重复投入。
较大的功能、公共 API 或架构改动，请先通过 Issue 与维护者讨论使用场景、方案和验证方式。

报告问题时，请提供预期行为、实际行为和最小复现步骤，注明项目版本或提交、操作系统与架构，以及相关的 Python、CANN、驱动版本、设备和传输类型。
涉及跨节点运行时，补充节点数、rank 配置及必要日志。
分享日志前请移除口令、令牌、私钥和个人信息。

## 环境与硬件要求

根据贡献内容选择环境，详细依赖和配套要求见[编译安装](./docs/zh/install_run.md)及 [README](./README.md)。

| 贡献或验证内容 | 环境要求 |
| --- | --- |
| 文档、代码阅读和评审 | 任意操作系统即可，无需 NPU 或专用网卡 |
| 单元测试 | Linux 开发环境和测试依赖；UT 使用 mock CANN 和 mock 设备接口，不能替代真机验证 |
| 编译构建和 whl/run 包打包 | Linux 开发环境，CMake、gcc、Python、pybind11 等编译依赖；无需 NPU |
| NPU 内存访问、设备传输和样例运行 | 对应昇腾设备、CANN、驱动及所需互联环境 |
| 跨节点分布式验证 | 多节点昇腾环境，网络互联及 MemFabric 底座已部署 |

UT 使用 mock CANN 和 mock SMEM/UBSIO 接口，可在无 NPU 环境下编译运行，但不能替代真机验证。
UT 的模拟接口验证与实际设备、网卡及多节点验证应分别记录。

可参考 [DevContainer 快速入门](./docs/zh/devcontainer_quickstart.md)准备远端昇腾开发环境。
该文档中的设备样例运行流程需要对应硬件，不作为普通 Linux 贡献的前置条件。
运行样例前，按[环境变量说明](./docs/zh/environment_variables.md)配置所需环境。
常用环境变量包括 `MMC_META_CONFIG_PATH`、`MMC_LOCAL_CONFIG_PATH`、`MEMCACHE_HYBRID_HOME_PATH`、`LD_LIBRARY_PATH`。

## 获取源码与构建

在 GitCode Fork 仓库后克隆自己的 Fork，按任务对应版本选择目标分支，再创建工作分支。

不确定目标分支时，请参考[开发与发布节奏原则][branch-policy]。

首次运行 UT 或构建测试工具前，在仓库根目录初始化子模块：

```bash
git submodule update --recursive --init
```

构建优先使用封装脚本，参数和安装步骤见[编译安装](./docs/zh/install_run.md)：

```bash
# 默认打包构建：RELEASE、Python ON、UBSIO ON
bash script/build_and_pack_run.sh

# 调试构建，复用已有 build/
bash script/build_and_pack_run.sh --build_mode DEBUG --incremental

# 构建 .run 包和 wheel，不构建测试工具
bash script/build_and_pack_run.sh --build_mode RELEASE --build_test OFF

# 不构建 UBSIO（SSD 后端）
bash script/build_and_pack_run.sh --build_ubsio OFF
```

`--build_ubsio OFF` 可在无需 SSD 后端时跳过 UBSIO 动态库构建；
`--build_cpp_example ON` 可在构建中包含 C++ 示例。
编译统一使用 `build_and_pack_run.sh`，不要直接调用底层 `build.sh`。
构建配置不等于已完成相应硬件上的运行验证。

## 验证改动

### 单元测试与覆盖率

代码修改先运行相关 UT，存在跨模块影响时扩大测试范围。
首次运行需准备构建及测试依赖。
UT 脚本使用 mock CANN 和 mock SMEM/UBSIO 接口，无需 NPU 即可编译运行。
完整覆盖率流程还需 `lcov` / `genhtml`。命令详情见 [AGENTS.md](./AGENTS.md)。

构建、UT 和 Fuzz 脚本已内置 `dos2unix` 兼容处理，未安装时会使用 `sed` 转换换行，无需另外安装或手动定义函数。

常用 UT 命令：

```bash
# 全量 UT：DEBUG 构建、运行 test_mmc_test 并生成覆盖率报告
bash script/run_ut.sh

# 按名称过滤运行（脚本会传递 --gtest_filter=*<名称>*）
bash script/run_ut.sh meta_service
```

覆盖率门禁：`src/memcache` 行覆盖率不少于 70%，分支覆盖率不少于 40%。
首次构建或配置变化时，会清理 `build/` 和 `output/` 后重新构建。

### 格式与预提交检查

在仓库根目录安装并启用预提交检查：

```bash
pip install pre-commit
pre-commit install --install-hooks
```

可针对改动文件运行 `pre-commit run --files <文件路径>`，全量检查入口为：

```bash
bash script/ci-pre-commit-pr.sh
```

全量脚本会检查和格式化整个仓库；若自动修改文件，请查看 diff，确认改动并重新检查。
配置见[预提交配置](./.pre-commit-config.yaml)和[预提交指南](./pre-commit/README.md)。

文档修改检查内容准确性、示例命令、仓库内路径和链接，并运行适用的预提交检查。
代码、API、样例或性能改动应补充相应验证；需要硬件但尚未完成的验证，请明确说明所缺环境和待验证内容。
用户或 CI 要求的检查仍需完成。

## 代码与文档规范

遵循周围代码的风格和现有依赖，详细项目要求见 [AGENTS.md](./AGENTS.md)。

- C/C++ 使用仓库的 [.clang-format](./.clang-format) 和 clang-format 18.1.8，缩进为 4 个空格，行宽不超过 120 字符。
- Python 使用 [Ruff 配置](./pre-commit/pyproject.toml)，目标版本为 `py310`，行宽不超过 120 字符。
- 函数保持单一职责，新增和修改的函数尽量不超过 50 行非空、非注释代码；控制块嵌套深度不超过 4 层。
- 新增错误返回路径应在根因发生处记录 ERROR 日志，包含失败原因和相关 rank、设备、资源、端点或错误码。
  上层仅透传已经记录的错误时，可以避免重复日志。
- 公共 API、参数或环境变量发生变化时，同步更新相应文档、样例和验证。
- 中文文档放在 `docs/zh/`，优先链接现有说明，避免重复维护大段安装步骤。

接口参考见 [C++ 接口](./docs/zh/memcache_c++_api.md)、[C API](./docs/zh/memcache_c_api.md)和 [Python API](./docs/zh/memcache_python_api.md)。

## 提交与 Pull Request

一个 PR 聚焦一个逻辑改动，提交说明清楚表达改动目的；较大的工作可拆成独立、可评审的步骤。
提交前检查 diff，避免带入与任务无关的修改、敏感信息，以及 `build/`、`output/`、`src/memcache/python/build/`、`*.egg-info` 等生成产物。

按照现有 [GitCode PR 模板](./.gitcode/PULL_REQUEST_TEMPLATE.md)填写说明和开发自检项，至少包含：

- 改了什么、为什么改，以及关联 Issue、里程碑或具体使用场景。
- 修改方案、影响范围，必要时说明兼容性和性能影响。
- 是否涉及 UT/ST；不需要时说明理由，需要时提供对应测试。
- 实际执行的验证命令、环境和结果；未完成的硬件验证应明确列出。
- 接口、参数、环境变量或行为发生变化时，对应资料已同步更新。

性能改动请提供同一环境下的前后对比、测量方法和必要配置，使结果可以复现。
提交后持续跟进评审反馈，解释取舍或按反馈更新实现，并在新增改动影响验证结果时重新运行相关检查。
讨论聚焦事实和技术问题，使用清晰、尊重的语言。

代码贡献遵循项目的 [Mulan PSL v2 许可证](./LICENSE)。
社区行为准则采用的许可见其[贡献归属](./CODE_OF_CONDUCT.md#贡献归属)。

[branch-policy]: https://gitcode.com/Ascend/memcache/wiki/%E5%BC%80%E5%8F%91%E4%B8%8E%E5%8F%91%E5%B8%83%E8%8A%82%E5%A5%8F%E5%8E%9F%E5%88%99.md

返回[项目首页](./README.md) · 阅读[社区行为准则](./CODE_OF_CONDUCT.md)
