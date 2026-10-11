# 平台与工具链矩阵

状态区分源码实现、本机运行和远端/原生验收；M0 工程入口通过不代表某个平台的 ThreadPool backend 已验收。

| 平台/架构 | 编译器边界 | 当前证据 | 尚未覆盖 |
|---|---|---|---|
| macOS x86_64 | AppleClang 14+；本机 AppleClang 17.0.0，CMake 3.29.2 | 工程与池的 Release、TSan、ASan+UBSan 本机回归已运行；M5 generic EventCount 及 RC11 边界见 M5 记录 | macOS 结果不验证 Linux `pthread_create` 属性或 Linux futex |
| Linux x86_64 | GCC 11+、Clang 14+ | M1 的 `pthread_create`、stack attribute 与 affinity 源码已实现；M5 futex 原生门禁已在 Linux x86_64（WSL2）完成：GCC 14 Release/ASan、Clang 18 TSan 及 generic control 的 fast/stress 全过，真实 EAGAIN/EINTR/spurious 与 join 后注册归零已验证（2026-10-11，见 M5 记录）。GitHub Actions 配置尚无可核验的远端运行结果 | 远端 Linux CI 结果；WSL2 证据仅覆盖 M5 正确性门禁，不扩展到 M6 物理 Linux 性能验收 |
| Linux ARM64 | GCC/Clang 的最低版本沿用 Linux 条目 | 无 ARM64 主机或云机资源 | 任何内存序放宽前的 ARM64 编译、运行和模型联合门禁 |
| Windows | MSVC 19.34+ 检查与导出宏分支已提供 | 无本机或 Windows CI 证据 | 静态/共享 consumer、DLL 加载、MSVC warning 与 sanitizer 支持；当前不列为已验证平台 |

## Backend 边界

- M1 Linux worker 创建使用 `pthread_create` trampoline 和 stack 属性；非零 stack 小于 `PTHREAD_STACK_MIN` 或 attr 设置失败时 warning 并使用默认栈。`pin_to_cores` 按允许逻辑 CPU 轮转，并在线程创建成功后 best-effort 调用 `pthread_setaffinity_np`；失败时 worker 保持不绑核，不使线程创建失败。未实现物理核拓扑优先。
- M1 generic backend 使用 `std::thread` 与 `std::condition_variable`；它记录并忽略 `worker_stack_size` 和 `pin_to_cores`。macOS 本机实际运行该 backend。
- 默认 CV 模式继续采用关闭期停车。M5 EventCount 已实现 Linux futex 与 generic 每槽 CV 两种后端，停止后不重新停车；本机只能验证 generic。Linux worker/futex 源码存在不等于原生验收通过。
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
| Linux futex | Linux 原生 worker backend 和 syscall 级交错 | M5 futex 正确性门禁已在 Linux x86_64（WSL2、GCC 14/Clang 18）完成，证据见 M5 milestone 与 `results/linux-m5-20261011/`；性能与 ARM64 属 M6 |
| 性能机器 | 固定物理 Linux CPU/亲和性/配额 | 2026-10-11 用户裁定本机 WSL2（kernel 6.18.40.1-microsoft-standard-WSL2、20 vCPU）作为 M6 验收环境，理由与风险见 benchmark-plan §2；ARM64 与物理 Linux 复查未变 |

GenMC probe 不接受仅靠非零退出或 `assert` 文本作为负例成功：正例需有非零 complete-execution 计数及 no-errors 完成标记；负例需有 Safety violation、指定断言表达式和源行号、非空 error graph。M3/M4/M5 项目协议已有有限 RC11 投影；EventCount 模型使用抽象 compare/park/wake，不执行 Linux syscall。工具版本及边界见 [`../tools/model/README.md`](../tools/model/README.md)。

M3 原语与 MPMC+CV 候选在 macOS x86_64 验证；Linux GCC/Clang 的双 backend CI 已配置、未原生执行。RC11 C11协议投影不覆盖 pthread ABI、C++全部语义或活性。默认保持mutex，见 [M3 milestone](milestones/M3.md)。

M4 Chase-Lev/MPMC+CV候选在macOS x86_64完成本机验证；Linux GCC Release/ASan与Clang Release/TSan工作窃取CI已配置，尚无远端结果。ARM64未执行。构建/模型/性能证据见 [M4 milestone](milestones/M4.md)，默认保持mutex/global。

M4含真实`atomic_thread_fence`，Linux TSan job改用Clang；GCC的[`-Wtsan`官方说明](https://gcc.gnu.org/onlinedocs/gcc-13.1.0/gcc/Warning-Options.html#index-Wtsan)将该特性列为不支持，并可能误报。本项目保留fence与`-Werror`，不关闭诊断；GCC正常Release/ASan继续进矩阵，GCC+TSan当前不列为已支持组合。Linux本机使用TSan preset时应明确选择`-DCMAKE_CXX_COMPILER=clang++`。Clang sanitizer通过也不能证明SC fence协议；RC11与平台回归仍独立。

M5 已完成：macOS x86_64/generic 开发验证之外，2026-10-11 在 Linux x86_64（WSL2，真实 Linux 内核与 futex syscall）以 GCC Release/ASan、Clang Release/TSan 和 generic control 完成 fast/stress 验收；用户裁定 WSL2 可作为 M5 正确性门禁的 Linux 原生证据，该裁定不扩展到 M6 物理 Linux 性能验收。Linux CI 的 GitHub Actions 配置仍无可核验远端结果。见 [M5 milestone](milestones/M5.md)。
