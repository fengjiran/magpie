# M0 平台与工具链矩阵

状态只表示 M0 工程入口的可用性，不代表线程池 backend 已实现或通过平台验收。

| 平台/架构 | 编译器边界 | M0 当前证据 | 尚未覆盖 |
|---|---|---|---|
| macOS x86_64 | AppleClang 14+；本机 AppleClang 17.0.0，CMake 3.29.2 | Release、TSan、ASan+UBSan 的 build 与 fast CTest 均已在本机运行；GenMC atomic/fence capability probes 在 RC11 下通过 | 通用线程 backend 尚未实现；macOS 结果不验证 Linux futex |
| Linux x86_64 | GCC 11+、Clang 14+ | GitHub Actions 配置了 GCC 三变体和 Clang Release job；本地没有 Linux executor | CI run 尚无可记录的结果；Linux `pthread_create` 与 futex 代码/原生 syscall 测试尚未实现 |
| Linux ARM64 | GCC/Clang 的最低版本沿用 Linux 条目 | 无 ARM64 主机或云机资源 | 任何内存序放宽前的 ARM64 编译、运行和模型联合门禁 |
| Windows | MSVC 19.34+ 检查与导出宏分支已提供 | 无本机或 Windows CI 证据 | 静态/共享 consumer、DLL 加载、MSVC warning 与 sanitizer 支持；当前不列为已验证平台 |

## Backend 边界

- 后续 Linux worker 创建使用 `pthread_create` 及线程属性，以落实可配置 stack size。
- generic backend 使用 `std::thread`，并明确记录不支持 worker stack size 配置。
- 两者均尚未实现。M0 只建立平台识别、最低工具链检查、C++20 feature probe 和跨平台导出宏。
- `MAGPIE_CACHE_LINE` 是唯一缓存行构建配置名，默认 64；CMake 对 16–4096 范围和 2 的幂进行校验。实际硬件 destructive-interference size 是后续测量/平台信息，当前默认值不是性能验收结论。

## 工具边界

| 工具/门禁 | 固定配置 | 当前状态 |
|---|---|---|
| CMake/C++20 | CMake 3.21+；GCC 11+、Clang 14+、AppleClang 14+、MSVC 19.34+ 初始边界 | 本机 CMake 3.29.2 与 AppleClang 17.0.0 的 C++20 feature probe 通过；Linux 在 Actions 配置，未本地执行 |
| GTest | release tag `v1.14.0`，测试依赖 | 本机找到 1.14.0 已安装 package，未触发 FetchContent；生产目标不链接 GTest |
| GenMC | 固定 release `v0.17.0`，commit `29b03a66402c4453fc77901ef3be90bb55707cd4`；显式 RC11；本机构建使用 LLVM 19.1.7 | 正例无错误并穷举 2 个 complete executions；relaxed 负例找到目标断言安全错误，完整 trace 和 DOT graph 已归档：[run.json](../results/model/genmc-20261003T041133.472502Z/run.json)。仅验证模型工具和 atomic/fence 输入能力，不覆盖任何项目协议或 C++20 全部语义 |
| Linux futex | Linux 原生 worker backend 和 syscall 级交错 | M0 未实现，且本机为 Darwin；留待 M5 原生 Linux 验收 |
| 性能机器 | 固定物理 Linux CPU/亲和性/配额 | 资源未安排；M6 前置 |

GenMC probe 不接受仅靠非零退出或 `assert` 文本作为负例成功：正例需有非零 complete-execution 计数及 no-errors 完成标记；负例需有 Safety violation、指定断言表达式和源行号、非空 error graph。该 capability probe 已在 macOS x86_64 实际通过；项目协议模型仍待 M4/M5。工具固定版本和语义边界见 [`../tools/model/README.md`](../tools/model/README.md)。
