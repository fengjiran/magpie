# 平台与工具链矩阵

状态区分源码实现、本机运行和远端/原生验收；M0 工程入口通过不代表某个平台的 ThreadPool backend 已验收。

| 平台/架构 | 编译器边界 | M0 当前证据 | 尚未覆盖 |
|---|---|---|---|
| macOS x86_64 | AppleClang 14+；本机 AppleClang 17.0.0，CMake 3.29.2 | M0 与 M1 的 Release、TSan、ASan+UBSan fast 通过；generic `std::thread` + condition_variable backend 实际运行；GenMC capability probes 在 RC11 下通过 | macOS 结果不验证 Linux `pthread_create` 属性或 Linux futex |
| Linux x86_64 | GCC 11+、Clang 14+ | M1 的 `pthread_create`、stack attribute 与 affinity 源码已实现；GitHub Actions 配置了 GCC 三变体和 Clang Release job；本地没有 Linux executor | 本轮尚无 Linux CI 结果；pthread ABI、stack/affinity fallback 和原生 syscall 尚待 Linux 执行 |
| Linux ARM64 | GCC/Clang 的最低版本沿用 Linux 条目 | 无 ARM64 主机或云机资源 | 任何内存序放宽前的 ARM64 编译、运行和模型联合门禁 |
| Windows | MSVC 19.34+ 检查与导出宏分支已提供 | 无本机或 Windows CI 证据 | 静态/共享 consumer、DLL 加载、MSVC warning 与 sanitizer 支持；当前不列为已验证平台 |

## Backend 边界

- M1 Linux worker 创建使用 `pthread_create` trampoline 和 stack 属性；非零 stack 小于 `PTHREAD_STACK_MIN` 或 attr 设置失败时 warning 并使用默认栈。`pin_to_cores` 按允许逻辑 CPU 轮转，并在线程创建成功后 best-effort 调用 `pthread_setaffinity_np`；失败时 worker 保持不绑核，不使线程创建失败。未实现物理核拓扑优先。
- M1 generic backend 使用 `std::thread` 与 `std::condition_variable`；它记录并忽略 `worker_stack_size` 和 `pin_to_cores`。macOS 本机实际运行该 backend。
- 两个 backend 均采用 M1 condition_variable 关闭期停车；M5 Linux futex/EventCount 仍未实现。Linux worker 后端代码存在不等于 Linux 原生验收通过。
- `MAGPIE_CACHE_LINE` 是唯一缓存行构建配置名，默认 64；CMake 对 16–4096 范围和 2 的幂进行校验。实际硬件 destructive-interference size 是后续测量/平台信息，当前默认值不是性能验收结论。

## 工具边界

M2 benchmark 使用 Linux/macOS 的 steady_clock 和 POSIX process/thread CPU clocks；Linux
显式 worker/producer affinity 为 opt-in，macOS 记录 unbound。当前仅有 macOS 的开发验证，
Linux benchmark 编译/运行、资源固定和物理机完整矩阵仍待执行。Windows benchmark 后端未提供，
不影响普通库构建。基线归档与状态见 [M2 milestone](milestones/M2.md)。

| 工具/门禁 | 固定配置 | 当前状态 |
|---|---|---|
| CMake/C++20 | CMake 3.21+；GCC 11+、Clang 14+、AppleClang 14+、MSVC 19.34+ 初始边界 | 本机 CMake 3.29.2 与 AppleClang 17.0.0 的 C++20 feature probe 通过；Linux 在 Actions 配置，未本地执行 |
| GTest | release tag `v1.14.0`，测试依赖 | 本机找到 1.14.0 已安装 package，未触发 FetchContent；生产目标不链接 GTest |
| GenMC | 固定 release `v0.17.0`，commit `29b03a66402c4453fc77901ef3be90bb55707cd4`；显式 RC11；本机构建使用 LLVM 19.1.7 | 正例无错误并穷举 2 个 complete executions；relaxed 负例找到目标断言安全错误，完整 trace 和 DOT graph 归档在本机 `results/model/genmc-20261003T041133.472502Z/`（不随仓库跟踪）。仅验证模型工具和 atomic/fence 输入能力，不覆盖任何项目协议或 C++20 全部语义 |
| Linux futex | Linux 原生 worker backend 和 syscall 级交错 | M5 未实现，且本机为 Darwin；留待 M5 原生 Linux 验收 |
| 性能机器 | 固定物理 Linux CPU/亲和性/配额 | 资源未安排；M6 前置 |

GenMC probe 不接受仅靠非零退出或 `assert` 文本作为负例成功：正例需有非零 complete-execution 计数及 no-errors 完成标记；负例需有 Safety violation、指定断言表达式和源行号、非空 error graph。该 capability probe 已在 macOS x86_64 实际通过；项目协议模型仍待 M4/M5。工具固定版本和语义边界见 [`../tools/model/README.md`](../tools/model/README.md)。
