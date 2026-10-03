# magpie

magpie 是一个面向 C++20 的固定 worker 工作窃取线程池。当前交付的是 M1 mutex 正确性底座：固定 worker、全局有界队列、拒绝策略、future、关闭与排空语义已实现。M3 已提供可选 MPMC + condition_variable 候选（默认仍为 mutex）；Chase-Lev 和 EventCount 尚未接入；当前版本不提供工作窃取快路径。

## 开发构建

需要 CMake 3.21 或更新版本，以及支持所需 C++20 标准库特性的编译器。工程检查 GCC 11+、Clang 14+、AppleClang 14+ 和 MSVC 19.34+；最低版本是配置检查边界，平台/编译器组合仍以 CI 和后续门禁的实际结果为准。Linux 与 macOS 是当前开发验证平台；Windows/MSVC 配置仅提供基础分支，尚未由 CI 或本机验证。多配置生成器（如 Visual Studio）通过 target compile definition 记录实际的 Debug/Release 配置。

```sh
cmake --preset release
cmake --build --preset release
python3 scripts/run_ctest.py --build-dir build/release --mode fast
```

三个互相独立的构建目录和变体：

```sh
cmake --preset release       # Release，不启用 sanitizer
cmake --preset tsan          # RelWithDebInfo + ThreadSanitizer
cmake --preset asan-ubsan    # RelWithDebInfo + AddressSanitizer/UndefinedBehaviorSanitizer
```

配置时默认构建测试（由标准 CMake 选项 `BUILD_TESTING` 控制）。GTest 仅作为测试依赖，优先使用精确版本 1.14.0 的已安装 CMake package；未找到时通过 CMake FetchContent 获取固定 release tag `v1.14.0`。生产库没有第三方运行时依赖。使用 `-DMAGPIE_BUILD_SHARED=ON` 可构建共享库；`MAGPIE_CACHE_LINE` 默认为 64，必须是 16 到 4096 之间的 2 的幂。Release 工程目标显式使用 `-O2`（MSVC 使用 `/O2`）；自有生产、测试与 consumer 目标启用 `-Wall -Wextra -Werror`（MSVC 使用 `/W4 /WX`），不向下游目标传播。

`fast`、`stress` 和 `soak` 均通过同一入口运行：

```sh
python3 scripts/run_ctest.py --build-dir build/release --mode fast
python3 scripts/run_ctest.py --build-dir build/release --mode stress
python3 scripts/run_ctest.py --build-dir build/release --mode soak
```

外层进程超时分别为 300 秒、3600 秒和 86400 秒；CTest 单测试超时由 CMake 注册 helper 设置。空标签会返回错误，避免把没有测试误报为通过。M1 包含 fast 池语义测试和一百万任务 ID 守恒 stress 用例；soak 门禁待后续里程碑加入。

## ThreadPool 使用

API 示例、拒绝策略、future 异常、同步关闭禁令、统计口径和 M1 限制见 [`docs/thread-pool.md`](docs/thread-pool.md)。

三个开发 preset 显式构建单独的 `magpie_test_support`，仅该静态测试库包含确定性交错 hooks；生产目标 `magpie` 始终不编译或导出测试 hook ABI。生产构建使用 `-DBUILD_TESTING=OFF -DMAGPIE_ENABLE_TEST_HOOKS=OFF`。测试构建中的提交门、drain 归零和构造回滚交错测试由 `MAGPIE_ENABLE_TEST_HOOKS=ON` 启用。

## 验证环境边界

M2 benchmark 的构建、负载矩阵、采样口径、资源控制、基线冻结与 checksum 重跑见
[`docs/benchmarks.md`](docs/benchmarks.md)。使用 `cmake --preset bench` 与
`cmake --build --preset bench --parallel 2` 构建独立 opt-in harness。
macOS 数据只供开发；M2 完整矩阵与正式冻结仍待固定物理 Linux 验收。

- 完整最低版本、架构和验证状态见 [`docs/platform-matrix.md`](docs/platform-matrix.md)。
- M1 Linux worker 创建代码使用 `pthread_create`/线程属性；macOS、Windows 等 generic 后端使用 `std::thread`。本机运行过 generic 后端；Linux 源码尚未由 Linux 编译或原生运行验收。
- Linux futex 是独立原生后端测试，macOS 的 generic 测试不能替代它。
- GenMC 固定 v0.17.0 的 RC11 atomic/fence capability probe 已在本机实际通过；日志、版本和 DOT graph 归档在本机 `results/model/genmc-20261003T041133.472502Z/`（不随仓库跟踪）。它验证模型工具输入能力，不覆盖项目原语；运行入口和边界见 [`tools/model/README.md`](tools/model/README.md)。
- ARM64 必须用实际 ARM64 主机或云机执行，x86_64 结果不能替代。

## 验证证据

每个里程碑的环境、revision、命令、结果和未关闭门禁记录在 `docs/milestones/Mx.md`；原始日志与性能数据归档在本机 `results/`（不随仓库跟踪）。新运行使用新的原始日志文件，不覆盖既有结果；提交报告时记录实际源码 revision、编译器、平台、构建变体和测试标签。M0、M1 与 M2 记录分别见 [`docs/milestones/M0.md`](docs/milestones/M0.md)、[`docs/milestones/M1.md`](docs/milestones/M1.md) 与 [`docs/milestones/M2.md`](docs/milestones/M2.md)。

许可证为 Apache-2.0，见 [`LICENSE`](LICENSE)。项目不承诺跨版本 ABI/符号兼容；发布说明将明确变更范围。

M3 的原语 API、候选构建和 weak try/关闭边界见 [docs/mpmc-queue.md](docs/mpmc-queue.md)，实际门禁状态见 [M3 milestone](docs/milestones/M3.md)。
