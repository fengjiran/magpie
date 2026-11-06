# magpie 全面系统性审核报告（2026-11-06）

- 审核对象：magpie 线程池仓库（/home/richard/project/magpie，HEAD `49be15b`，工作区 clean，`git diff --check` 通过）。
- 审核范围：代码质量、功能完整性、安全性、性能与稳定性、文档与配置五维；结论输出为问题清单 + 可落地优化指引。
- 方法：逐文件静态阅读全部生产源码（include/magpie、src 全部 8 个 .cpp/.hpp）、构建配置（CMakeLists、presets、cmake/*）、测试基建（tests/、scripts/）、benchmark 基建（bench/）、模型工具（tools/model/），对照主设计、ADR-001/002/003、test-plan、benchmark-plan、全部里程碑记录；并在本机实际执行配置、构建与测试取证（见 §7）。
- 判定口径：已验证事实、推断、建议分开标注；正确性/所有权/关闭活性优先于性能；历史审核结论不等于当前状态，一律以当前代码与最新里程碑记录为准。
- 处置记录（2026-11-06）：H-1 与 L-1 已按 §8 P0 第 1 项就地修复——README.md「验证环境边界」「验证证据」及末尾 M4/M5 段落更新为最新状态，补 M6 段落（gate 2/4/7、五宏标定默认与白名单）与新 preset/TSan Clang 说明；全部链接目标校验存在，`git diff --check` 干净。其余条目维持原判定。
- 处置记录（2026-11-06，第二轮）：P0 其余条目已处理——L-2（thread-pool.md 注明 submit_async 异常双重投递）、L-3（deque/MPMC 溢出保护防御注释）、L-4（submit_task 防御分支注释）、L-5（spin_wait 回退注释）、L-7（M1.md 补后续验收指引链接）、L-10（check_design.py 缺编译器友好提示，已实测 PASS 与错误提示路径）。L-8/L-9 经用户确认**豁免**：GTest 保持 release tag `v1.14.0`（仅测试依赖、版本已固定），ci.yml 的 actions 保持 major tag（该 CI 尚无远端运行记录）；正式启用远端 CI 时再评估 commit sha 锁定，判定理由登记于此。其余条目维持原判定。

## 1. 总体结论

- 代码库整体质量**高**：所有权模型（RAII TaskOwner/ExecutionScope/OwnerScope）、pending/gate 协议、拒绝策略、关闭握手与主设计及 ADR 逐条对应；release 生效的 fail-fast 与异常路径收口完整；测试体系具备三层证据（守恒 stress / 确定性缝 / 有界 RC11 模型）+ watchdog + 变异负例，是本项目最突出的资产。
- 本轮全绿取证：默认 release、asan-ubsan、workstealing、eventcount（futex）四个配置的 fast 与 stress 全部通过（详 §7）。**未发现严重级问题**（在当前已实现范围内未发现正确性/所有权/关闭活性缺陷）。
- 主要风险集中在"已登记未收敛"项：M6 性能门禁 gate 2/4/7 开放（多 worker 亚微秒扩展 0.485×、低载 p99 比值 1.060、CPU 放大 13.8×）、远端 CI/Windows/ARM64/soak 缺证据，以及对外门面 README 与最新证据脱节。
- 等级分布：高 1、中 4、低 12。以下按用户要求的五个维度组织。

## 2. 问题清单总表

| 编号 | 等级 | 维度 | 位置 | 摘要 |
|---|---|---|---|---|
| H-1 | 高 | 文档 | README.md L49、L64 | README 仍称"Linux 源码尚未经 Linux 编译或原生运行验收"、"M5 Linux 原生验收保留待完成"，与 M5(2026-10-11)/M6-review-fixes(2026-11-05) 证据矛盾，对外门面信息错误 |
| M-1 | 中 | 功能/配置 | .github/workflows/ci.yml | CI 未固定 clang 版本（`compiler: clang++` 依赖 runner 滚动版本），且全部 job 仍无可核验远端运行结果 |
| M-2 | 中 | 性能/功能 | docs/milestones/M6.md §4/§14；docs/milestones/M6-review-fixes.md | gate 2/4/7 开放：多 worker 亚微秒 T8/T1=0.485、CPU 放大 13.8×、低载精确 p99 比值 1.060；M7 首版交付与候选默认切换的前置未关闭 |
| M-3 | 中 | 功能 | src/thread_pool.cpp submit/submit_mpmc | mutex 与 MPMC 两条提交分发路径结构重复、rejected 记账时点语义错位（弱 try 失败 vs 真满），缺少跨后端口径对照测试 |
| M-4 | 中 | 功能 | tests/CMakeLists.txt；测试计划 §3 | T3 soak 层无注册用例（--mode soak 按设计返回 3）；小时级 soak 与 M7 release 用例待补 |
| L-1 | 低 | 文档 | README.md | 未提及 M6 状态、五宏标定参数（MAGPIE_BULK_LIMIT/STEAL_CAP/SPIN_LIMIT/YIELD_LIMIT/VICTIM_TRIES）与 workstealing/eventcount 等新 presets |
| L-2 | 低 | 文档 | docs/thread-pool.md；src/thread_pool.cpp run_one | submit_async 的 callable 异常既进入 future 又投递 exception_handler（双重投递），文档未明示 |
| L-3 | 低 | 代码质量 | include/magpie/chase_lev_deque.hpp try_push/steal、include/magpie/mpmc_queue.hpp | 若干索引上限 fail-fast 分支在当前不等式顺序下不可达（防御性），无注释说明保留意图 |
| L-4 | 低 | 代码质量 | src/thread_pool.cpp ThreadPool::submit_task | impl_==nullptr 防御分支对已构造对象不可达（仅构造抛异常前窗口），建议注释保留意图 |
| L-5 | 低 | 代码质量 | src/spin_wait.hpp | 未知架构回退 `atomic_signal_fence` 仅为编译屏障、非硬件退避，无注释说明 |
| L-6 | 低 | 代码质量 | include/magpie/build_config.hpp.in；CMakeLists.txt | `MAGPIE_ENABLE_WORK_STEALING`（公共生成头 0/1）与 `MAGPIE_USE_WORK_STEALING`（私有编译定义）双命名并存易混 |
| L-7 | 低 | 功能 | docs/milestones/M1.md L5 | M1 状态行停留在 2026-10-03 表述（Linux pthread 未经验证），与后续 M5/M6 Linux 验收脱节，缺更新指引链接 |
| L-8 | 低 | 安全 | tests/CMakeLists.txt | FetchContent 拉取 googletest 固定 tag v1.14.0（GIT_SHALLOW），未记录 commit hash；tag 理论可移动 |
| L-9 | 低 | 安全/配置 | .github/workflows/ci.yml | actions/checkout@v4、setup-python@v5 用 major tag 未固定 commit sha |
| L-10 | 低 | 功能 | design/validation/check_design.py | 默认依赖 clang++；环境无 clang 时需显式 CXX=，辅助检查入口易失效 |
| L-11 | 低 | 性能/稳定性 | docs/event-count.md §关闭与统计 | 关闭期单次 drain 返回后统计未完全稳定、部分 worker 可能仍在退出——已文档化但外部使用者易误读，建议在 thread-pool.md 引用处重复一句 |
| L-12 | 低 | 性能/稳定性 | src/worker_thread.cpp | generic 后端 `worker_stack_size`/`pin_to_cores` 静默忽略（仅 stderr 警告）；API 侧无返回值/查询通道确认实际生效值（best-effort 已文档化） |

## 3. 代码质量审核

### 3.1 规范遵循

- 命名一致（worker_loop/submit_task/dec_pending/transfer_batch），中文注释解释 why/invariant（如 transfer_batch 三条容量证明、dec_pending 与 drain mutex 配对理由），未发现逐行复述式注释。
- warnings-as-errors 严格生效：MagpieToolchain/MagpieOptions 双函数校验，`-Wall -Wextra -Werror`（MSVC `/W4 /WX`），四个配置实际构建全部零告警通过。
- 目录职责边界清晰：header-only 原语（mpmc/deque）不含池逻辑；池层调度/通知在 src/thread_pool.cpp；平台分支收敛在 worker_thread.cpp 与 event_count_{linux,generic}.cpp，未发现平台条件散布调度代码。
- 测试依赖与生产隔离：GTest/FetchContent 仅测试侧；`magpie_test_support` 独立静态库承载注入缝；生产 `magpie` 不编译 hook ABI；`MAGPIE_ENABLE_TEST_HOOKS` release 剔除（`#define MAGPIE_TEST_HOOK (void)0`）。
- `git diff --check` 干净、无 TODO/FIXME/XXX 残留。

### 3.2 结构与可维护性

- pimpl + 分层清晰：ThreadPool 公共薄模板（submit/submit_async）→ TaskOwner 托管 → Impl 提交协议；ExecutionScope/OwnerScope TLS 链正确覆盖 worker、CallerRuns、异常回调、被丢弃任务与捕获对象析构。
- 统计布局经过 M6 §8 按 §13 分行重构并有 Clang layout 验证；per-worker WorkerCtx 含独立缓存行统计，避免跨核争用。
- 发现 M-3：mutex 与 MPMC 两条提交路径重复实现逻辑（加 gate→检查 stopping→加 pending→入队/降级→统计/回滚），语义依靠人工保持对齐。本轮逐条核对与主设计 §770/§771 口径一致，但缺少跨后端对照的自动化测试。
- 建议：
  1. 为新口径（rejected 每次提交最多一次、discarded 只计真实销毁、CallerRuns 计入 submitted）增加一个在两个 backend 上参数化运行的跨后端语义用例（rejected/discarded 记账对照、恒等式），防止将来一侧改动漂移。
  2. 或在 submit 分叉处加交叉注释引用另一路径与主设计 §8.4，明确两侧必须同步修改。

### 3.3 可读性/注释准确性

- 多为有效注释；发现少数失效的防御检查（L-3、L-4、L-5），建议各加一句"防御性/当前控制流不可达，勿删"。
- 命名上 L-6 两个 WORK_STEALING 宏并存，建议后续统一命名（如公共生成头改 MAGPIE_HAVE_WORK_STEALING），或在 build_config.hpp.in 中注释映射关系。

## 4. 功能完整性审核

### 4.1 声明功能 versus 实现（本轮逐项核对）

| 声明（来源） | 实现状态 |
|---|---|
| 固定 worker + 有界全局队列（mutex 默认） | ✓ src/thread_pool.cpp mutex ring |
| MPMC 候选（weak try、reservation hole、DiscardOldest 不跳过未发布头、DISCARD_RETRY_LIMIT=2） | ✓ 与 ADR-003、docs/mpmc-queue.md 一致 |
| Chase-Lev 候选（单元素 steal、双侧 SC fence、b+1 稳定恢复、失败候选不读、全批转移前不执行用户代码、保留执行项） | ✓ 与 ADR-001 v2.1 一致 |
| EventCount 候选（独立 WaitSlot、Idle/Waiting/Signaled 状态字、epoch 先推进、futex PRIVATE、generic 每槽 mutex/cv、关停期重新停车 + 完整退出复查 + pending/gate 最终通知） | ✓ 与 ADR-002 v3.2 一致 |
| 拒绝策略 CallerRuns/Abort/DiscardOldest（worker 内强制 CallerRuns、外部按策略、brokен_promise） | ✓ |
| shutdown（停止接收、等提交门归零、不等待已接受任务）/drain（pending==0 观察点）/析构（shutdown→drain→join） | ✓ |
| 执行帧禁令（本池上下文内 shutdown/drain 抛 logic_error、析构 terminate；覆盖嵌套链与捕获对象析构） | ✓ |
| 生命周期契约（Task 恰一个最终执行者/丢弃者；pending 先加后减统一 dec_pending；1→0 持 drain mutex 通知） | ✓ |
| 统计口径（submitted 含 CallerRuns、rejected 每次提交最多一次、discarded 真实销毁数、worker/inline 分开、wakes/wake_threads/wait_* 语义） | ✓ 与主设计 §13、§770–784 一致 |
| 构造回滚（部分启动失败 → stopping→notify_all→join→重抛） | ✓ ConstructorFailureRollback 守护 |
| Linux pthread 栈/亲和性、generic std::thread 降级 | ✓ 与 platform-matrix Backend 边界一致 |
| M6 五宏标定（默认 BULK=32/STEAL=64/SPIN=1024/YIELD=8/VICTIM=1，CMake 枚举白名单） | ✓ 与主设计 §16.2 一致 |

### 4.2 边界与异常分支

- 容量规范化（≥16、2 的幂、溢出 fail-fast/抛异常）、worker 数上限、pending/gate/epoch/index 上限 fail-fast、null 载荷 fail-fast：实现齐全且 release 生效。
- 异常回调再抛收口（report_unhandled_exception + terminate）、worker 主循环 catch-all terminate、packaged_task 构造失败在提交计数前传播。
- 已知未覆盖（均已在记录中显式登记，不是隐藏缺陷）：Windows 零验证；ARM64 零资源；远端 CI 零可核验结果；soak(T3) 无注册用例；M6 gate 2/4/7 开放；W2.7 内存序放宽未开展。

## 5. 安全性审核

- 输入面：公共 API 全部内部校验（Options 枚举、容量、栈、payload 非空、概念约束 TaskCallable），未发现输入验证缺陷；库无文件/网络/环境解析路径，攻击面极小。
- 并发安全：无锁原语的候选锁存先于 CAS、失败候选绝不读取（R1/R2 守护）；TSan 历史全绿（本轮环境缺 TSan 工具链，见 §7 限制）。
- 敏感数据：无用户数据处理、无日志泄露面（stderr 仅诊断）。
- 依赖供应链：
  - L-8：googletest 仅测试依赖、tag 固定 v1.14.0，建议补 commit sha 锁定（FetchContent `GIT_TAG` 可填 40 位 sha）。
  - L-9：GitHub Actions 用 major tag，建议固定 commit sha。
  - 生产库无第三方运行时依赖 ✓。
- 平台 ABI：Linux futex 对 `atomic<uint32_t>` 的尺寸/对齐/lock-free 有编译期 static_assert，但标准不保证表示与 `uint32_t` 一致——已在 ADR-002 §5 与 platform-matrix 明示为"构建检查 + Linux ABI 依赖"，工程上可接受，属已知边界而非缺陷。
- C++20 atomic wait 的运行时行为依赖标准库实现质量（编译探测只验语法不验行为），老平台可能退化为忙等——建议在 platform-matrix 的 Backend 边界补充一句运行时行为边界说明。

## 6. 性能与稳定性审核

### 6.1 已登记的开放门禁（主要风险，非本轮新发现）

- gate 7 / gate 2（M-2）：三档 payload 下 T8/T1 完成速率 0.485/1.200/3.093、每叶 CPU 放大 13.8×/5.6×/2.3×；软件采样指向 pending/gate/submitted 共享 RMW 邻域（约 53%）与 pause 循环（约 15%），主因尚未证实的迁移通知旧表述已撤回。**建议下一步按 M6-review-fixes §归因与边界 的结论继续"共享元数据 vs 运行期退避"交叉定位**，不预设迁移通知为根因。
- gate 4：低载（10% 到达率）精确 p99 比值 1.060 超 1.05，90% 点比值 0.432 优势因双侧拒绝不可比；低载劣化机制待解释。建议补无拒绝的低载区间多档测量，定位候选在低载的额外共享成本（epoch RMW + 扫描 vs CV 的锁）。
- 这些门禁直接影响 M7 首版交付判定与候选默认切换，但不能因此提前做 W2.7/W3 优化——按数据单独立项符合 AGENTS §7。

### 6.2 资源泄漏与稳定性

- 未发现资源泄漏：RAII 全覆盖（unique_ptr/pimpl/HandlerHolder 析构 scope/WorkerThread join 幂等）；LSan 随 ASan 通过。
- 错误恢复：构造部分失败回滚、pthread 属性失败降级为默认栈、affinity best-effort 不使构造失败、异常回调收口——机制完整。
- 已知代价转嫁登记充分：Task 仍每项 new（alloc 成本）、pending/gate/submitted/epoch 共享 RMW、EventCount O(N) 通知扫描、victims 列表 O(W²) 空间——均在文档与 ADR 中如实声明，无虚假"零共享"宣传。

### 6.3 实测（本轮）与记录一致性

- 本机 20 vCPU WSL2：默认 release 配置的空任务历史压力点与 M6 记录一致（行为面），本轮未做性能测量（审核不改变性能结论）；性能判定引用 M6-review-fixes 的归档数据。

## 7. 本轮实测取证（2026-11-06，本机）

环境：Linux 6.x WSL2、x86_64、GCC 14.2.0、CMake 3.28.3、Python 3.12.3；因 TraeCode 沙箱注入过旧 `libstdc++` 的 LD_LIBRARY_PATH，命令统一以 `LD_LIBRARY_PATH=` 前缀清空后执行（与项目 M5 记录的环境注意事项一致）；ASan 沿用 `ASAN_OPTIONS=verify_asan_link_order=0`。

| 配置 | fast | stress | 备注 |
|---|---|---|---|
| release（mutex 默认，18 套件） | 18/18 | 3/3 | 含消费者、watchdog 探针、原语、池级全套装 |
| asan-ubsan（GCC） | 18/18 | 3/3 | LSan 全开 |
| release-workstealing（CV+WS） | 18/18 | — | fast 五分钟内全绿 |
| release-eventcount（auto=futex） | 19/19 | 3/3 | 含 Linux futex 语义套件与池级关闭/迁移回归 |
| tsan（Clang） | 未执行 | 未执行 | 环境无带 compiler-rt TSan 运行库的 Clang（自定义 Clang 22 未构建 libclang_rt.tsan；系统无 clang-18）；GCC+TSan 为项目明确不支持组合（GCC -Wtsan 对 atomic_thread_fence 不支持且 -Werror）。历史 Clang 18 TSan 全绿证据见 M5/M6-review-fixes 归档，不以本轮缺陷计 |

结论：四个可用配置 fast+stress 全绿，与 M6-review-fixes（2026-11-05）记录的九配置矩阵结论一致；沙箱日志中的子进程 abort/文件告警来自死亡测试的正常 terminate 与沙箱自身 crashpad 噪音，ctest 退出码均为 0。

## 8. 优化指引（可落地 backlog）

### P0（本轮可直接执行，无协议风险）

1. H-1：修订 README「验证环境边界」与「验证证据」两节——改为引用 platform-matrix 与 M5/M6 的最新状态（Linux/WSL2 futex 门禁已完成、M6 进行中 gate 2/4/7 开放），删除"Linux 尚未验收/保留待完成"的过时表述；同步补 M6 段落与五宏标定参数说明。
2. L-1：README 增补新 presets（release-mpmc/workstealing/eventcount 系列）与 M6 标定宏一行说明。
3. L-2：thread-pool.md 补充一句"submit_async 的异常同时进入 future 与 exception_handler（若配置）"。
4. L-3/L-4/L-5：为不可达防御分支与 spin 回退补 one-line 注释（不动逻辑）。
5. L-7：M1.md 顶部状态行加"（后续 Linux 编译/运行与 futex 门禁见 M5/M6 与 platform-matrix）"指引。
6. L-8/L-9：FetchContent GIT_TAG 改 40 位 commit sha；ci.yml 锁定 action commit sha（或记录豁免理由）。
7. L-10：check_design.py 输出友好的环境提示（当前报错已含 CXX 说明，改为提前检测）——可选。

验收：`git diff --check`、README 建链检查、release fast 全绿不变。

### P1（下一里程碑内，需测试/小设计）

8. M-3：新增跨后端 rejected/discarded/submitted 记账对照用例（mutex/mpmc 参数化），在 test-plan §4.5/§6 登记；验证 DiscardOldest 重试成功路径的 rejected/discarded 计数与主设计 §770/§771 一致。
9. L-6：统一 WORK_STEALING 宏命名（公共头/私有定义），一次全仓替换 + 全配置重建。
10. L-11/L-12：docs 补充关闭期统计稳定性提示；评估是否在 BuildInfo/Stats 暴露实际 stack_size/pinning 生效状态（若立项需走主设计 §6.2 修订）。

### P2（性能门禁收敛，按数据决策，勿预防性重构）

11. M-2/gate 7：按 M6-review-fixes §归因 继续交叉定位（共享 RMW 分项 vs sleep/wake 退避），产出机制证明后选择单机制对照实验（如 submit 路径分项采样、迁移通知与普通发布通知分开统计）；
12. M-2/gate 4：低载多档到达率（10%/30%/50%）无拒绝窗口重测 p99 与拒绝，解释低载 1.06 的机制来源；
13. M-1/M-4：远端 CI 首跑与工具链固定（锁定 clang 版本安装步骤）；M7 前补小时级 soak 用例（按 test-plan T3 后缀注册）；
14. 平台债务：ARM64 主机/云机预算与 Windows 验证路径在 M7 前决策记录（继续豁免还是采购资源）。

## 9. 引用依据

- 主设计 §5.3/§7/§8/§13/§14.1/§16.2：[design/magpie设计方案.md](../design/magpie设计方案.md)
- ADR-001/002/003、test-plan §4.5/§6、benchmark-plan §2/§5
- 里程碑：M5.md（2026-10-11 完成）、M6.md（进行中，gate 2/4/7）、M6-review-fixes.md（2026-11-05）、M1–M4 状态行
- 本轮位置引用：README.md、src/thread_pool.cpp、src/event_count*.cpp、src/worker_thread.cpp、src/spin_wait.hpp、include/magpie/*.hpp、tests/CMakeLists.txt、.github/workflows/ci.yml、CMakePresets.json、cmake/MagpieOptions.cmake