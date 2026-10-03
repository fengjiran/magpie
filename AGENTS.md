# magpie 项目协作规范

本文件适用于整个仓库，指导代码实施、设计审核、测试与性能研究。当前用户明确要求优先于本文件的默认约定；工具权限和环境约束仍按当前会话执行。不要从这些规范推导出额外的确认流程，已授权且明确的工作直接完成。

## 1. 项目定位与工作方式

magpie 是面向 C++20 的固定 worker、高性能工作窃取线程池。正确性、ownership/lifetime 与关闭活性先于性能优化；性能收益必须有机制解释和可复现数据。

- 默认中文交流，保留标准技术术语、API、类型名和函数名。
- 明确区分已验证事实、推断、建议与待验证项；不默认认同有问题的设计。
- 复杂问题按“问题 → 约束/invariant → 现有设计 → 方案 → trade-off → 推荐”分析。
- 优先最小必要修改、已有接口与职责边界，不做无关重构，不增加没有依据的 abstraction 或微优化。
- 注释解释 why、constraint 和 invariant，不逐行复述代码；不以 placeholder 冒充功能实现。
- 软件/工具升级、内存模型及平台细节不确定时查证一手资料；不能把硬件直觉当作 C++ 标准保证。
- 只在用户要求或当前授权包含委派时使用子代理；明确任务范围与文件职责，共享工作区中的修改不得互相覆盖。

## 2. 开始任务前阅读什么

先核对实际代码、工作区状态和当前里程碑，按任务读取相关文档：

| 来源 | 用途 |
|---|---|
| [README.md](README.md)、[平台矩阵](docs/platform-matrix.md) | 构建入口、平台与工具链实际支持状态 |
| [实施计划](design/implementation-plan.md)、`results/milestones/Mx.md` | 当前范围、前置、交付与验收状态 |
| [主设计](design/magpie设计方案.md) | API、所有权、调度、关闭与统计的协议约束 |
| [ADR-001](design/chase-lev-deque-adr.md) | 单元素 Chase-Lev、屏障与池层 fallback |
| [ADR-002](design/event-count-adr.md) | 每 worker WaitSlot、注册与关闭握手 |
| [ADR-003](design/mpmc-queue-adr.md) | MPMC 载荷、弱 try 契约与 reservation hole |
| [测试计划](design/test-plan.md) | 正式用例、测试缝、oracle 和平台门禁 |
| [benchmark 计划](design/benchmark-plan.md) | 资源、基线、指标与归因规则 |
| [修复对照](design/review-fixes-2026-10-03.md) | R1–R7 的处置及回归要求 |

主设计定义当前协议，ADR 解释决策与证明义务。发现代码、主设计或 ADR 冲突时，指出冲突并修正相关来源；不得静默选择对局部实现方便的口径。设计骨架不等于已实现代码，历史审核不等于当前仍有同一缺陷。

创建本文件时的状态快照（2026-10-03）：M0 工程与本机工具探针通过，远端 Linux CI 尚待验收；ThreadPool、MPMC、deque 和线程/停车后端尚未实现。后续任务以最新文件及证据为准，不能永久沿用这一快照。只实施本轮请求的里程碑，不擅自推进下一阶段或引入 W3 特性。

## 3. 目录与职责边界

- `include/magpie/`：公开头、薄模板及适合内联的原语。模板只做类型相关包装，池化协议放非模板实现。
- `src/`：池主体、公共非模板函数、线程和停车平台实现。按模块拆文件，避免平台条件散布调度代码。
- `cmake/`、`CMakePresets.json`：工具链、配置校验、生成头和构建变体。
- `tests/`：单元、确定性交错、池级集成及独立消费者；测试依赖不得进入生产运行时。
- `bench/`：测量 harness 与负载；benchmark flags/依赖不得污染生产库。
- `tools/model/`：固定模型工具的输入、运行与结果判定。
- `design/validation/`：设计骨架辅助检查，不能替代真实库测试。
- `design/`、`docs/`、`results/`：协议、使用/平台说明和可追溯证据。
- `build/`：可再生成的构建产物和开发工具，不存放唯一验证证据或待提交实现。

Chase-Lev 只负责有界 push/pop/steal；全局转投、策略、通知、执行和统计属于池层。队列不执行用户代码，不管理 future，不自行销毁 Task。

## 4. 不得破坏的并发与生命周期约束

### 4.1 Task、计数与异常

1. 一个接受的 Task 恰有一个最终执行者或显式丢弃者。队列间只传 Task*，不复制/移动任务本体；失败认领不转移所有权。
2. 提交包装用 RAII 托管至发布或内联执行。`pending++` 必须在任务可执行发布之前；完成、丢弃与拒绝回退统一递减，不能散落计数操作。
3. 正常执行和丢弃先完成回调/Task 销毁，再递减 pending；1→0 的通知持 `drain_mtx`，与 drain 谓词等待配对。
4. 用户代码只在队列操作完全返回后执行。捕获引用不自动延长引用对象 lifetime；存储 callable 的构造能力、调用 value category 与 noexcept 析构约束必须一致。
5. 裸任务异常进池级回调，packaged_task callable 异常进 future。异常回调再抛要收口；任何异常不能逃逸 worker 主循环。日志与清理路径应遵守非抛约束。
6. DiscardOldest 仅删除当前可领取的全局头项，销毁 packaged_task 后 future 为 broken_promise；不能静默丢项或宣称删除全池最老任务。

### 4.2 本地队列与工作窃取

- 只有 owner 可 try_push/pop；其他线程只能 steal。owner TLS 同时核对池实例，跨池提交不能错入另一池的 deque。
- 使用单元素 steal；池层重复单项获取，每项有独立 CAS。禁止恢复“一次 CAS 认领半批”的旧算法。
- 保留当前完整的双侧 SC fence/发布协议。CAS 失败会改写 expected，最后项 pop 的 bottom 恢复使用稳定的 `b+1`。
- 槽是原子指针；候选在 CAS 前锁存，CAS 失败后丢弃，绝不解引用失败候选。
- 本地满只把当前新任务转投全局，全局仍不可入则 CallerRuns；不搬运旧半批，不恢复 overflow 暂存或失败分区循环。
- 批接收量按本地容量钳制；完整转移前不执行用户代码。不得把 BATCH_CAP 当成任意迁移任务量的上界。
- `0≤bottom−top≤capacity` 仅在受控稳定边界检查。pop 中间态及两个独立原子的任意快照不能套用此断言。

### 4.3 MPMC 与内存序

- `MPMCQueue<T>` 存 T，池实例化 `Task*`，不意外生成 `Task**` API。
- 先查 slot seq，再 CAS 认领 position；认领后的读写不可抛、分配或回调，seq acquire/release 保护发布与复用。
- try 失败表示当前不可取得槽，不证明逻辑集合严格空/满；producer 未发布和 consumer 未释放窗口都必须覆盖。
- position 顺序、slot 发布、任务开始/完成是不同事件。不能宣称全池 FIFO 或严格线性化 FIFO 的 lock-free progress。
- 不允许索引/epoch 无约束回绕或有符号溢出。配置错误以异常报告；协议上限/内部不变量的 fail-fast 在 release 也生效。
- 内存序放宽前给出标准模型论证、对应模型检查与回归、ARM64 和同日性能证据。`seq_cst` 不自动证明算法正确，TSan 全绿也不证明 atomics 协议正确。

### 4.4 等待与关闭

- 每 worker 的 WaitSlot 独立且地址稳定，word 为 Idle/Waiting/Signaled，不是截断的 epoch。禁止等待者把旧 key 写回共享睡眠字。
- 注册先于最后的 stopping/任务探测，注册后再次检查 stopping，epoch 复查先于阻塞。所有路径配平注册；注销之前不执行认领任务。
- 通知先推进 epoch，再标记 Waiting→Signaled，最后 wake；队列发布先于通知。不能因注册数近似、局部正在执行或深度判断而私自省略通知。
- shutdown 的同步等待、drain 和析构禁令覆盖本池所有 ExecutionFrame：worker、外部 CallerRuns、fallback、异常回调和捕获对象析构；检测完整嵌套链，不只检查 owner 身份或最顶层帧。
- 本池任务不得阻塞等待本池 future。没有 helping wait 时，不用示例或内部重入绕过这一契约。
- worker 退出按 stopping→提交门→pending 裁决；提交门覆盖 CallerRuns。析构需要外部 lifetime 管理，门协议不能使并发销毁对象上的成员调用合法。
- drain 保证锚定成功观察 pending=0 的时刻；持续提交可能使它不返回，返回瞬间仍可能已有新提交。
- 构造先完成全部 victim/WaitSlot 上下文再启动线程；部分启动失败必须 stopping→notify_all→join→重抛。不访问未构造对象，不以“线程自然退出”代替通知。
- Linux 用 pthread_create 属性落实 stack_size，generic 用 std::thread；std::thread 不接受 pthread_attr。futex word 的 ABI/对齐、错误与 syscall 窗口在 Linux 原生验证。

## 5. 构建与检查入口

以实际 CMake/preset 为准；当前工程 C++20、CMake≥3.21，最低工具链及已验证平台见平台矩阵。自有目标启用严格警告并将 **warning 视为 error**；不要用全局关闭诊断掩盖问题。

从仓库根目录运行：

```sh
cmake --preset release
cmake --build --preset release --parallel 2
python3 scripts/run_ctest.py --build-dir build/release --mode fast

cmake --preset tsan
cmake --build --preset tsan --parallel 2
python3 scripts/run_ctest.py --build-dir build/tsan --mode fast

cmake --preset asan-ubsan
cmake --build --preset asan-ubsan --parallel 2
python3 scripts/run_ctest.py --build-dir build/asan-ubsan --mode fast
```

- TSan 与 ASan+UBSan 用独立目录，不叠加；Release 自有目标实际生效 `-O2`，生产静态/共享消费都要保留。
- `BUILD_TESTING` 控制测试；GTest 固定版本仅用于测试。生产库保持无第三方运行时依赖，新增开发依赖要有必要性和可复现版本记录。
- `MAGPIE_CACHE_LINE` 是唯一缓存行构建配置，使用现有范围/幂校验；不能凭默认 64 字节声称硬件已测量。
- fast/stress/soak 使用同一 runner。当前没有适用用例的标签返回错误，不算通过；新增测试按现有 helper 注册标签与超时。
- 不通过禁用断言、弱化 sanitizer、删除回归或跳过错误制造“全绿”。Release 的正常 NDEBUG 配置不替代必须在 release 生效的协议不变量/生命周期 guard。TSan 下不运行 fork/death 用例，按测试计划用独立验证。
- 按变更选择必要检查：文档做链接/一致性检查；构建/公共头验证适用消费者；协议/所有权变更运行对应正式测试与 sanitizer。已通过的检查不无依据反复扩大，新增失败/变更再补验证。
- 结束前运行 `git diff --check`，检查变更范围和用户原有文件。

## 6. 测试与模型证据

并发缺陷优先建立确定性暂停/释放回归，明确 Task id、认领位置、执行/丢弃集合、状态和失败现场，再辅以随机 stress。固定 seed 只复现 op 序列，不复现 OS 调度；replay 需要交错轨迹。

修改相关协议必须保留对应 R1–R7 守护：连续 owner pop、最后项 CAS 失败、本地满/容量钳制、独立等待字、晚注册关闭、CallerRuns/嵌套执行帧控制。测试缝唯一注册表在 test-plan；生产配置剔除，不用于业务回调。

MPMC 历史 oracle 允许 weak try 的 unavailable 结果，成功 id 与位置仍严格守恒；不可用严格 FIFO 模型误报，也不可用宽松模型隐藏成功消费错误。完全 quiescent、未发生统计饱和时校核 submitted=completed+discarded、pending=0，rejected 含 CallerRuns 不减入恒等式。

活性测试必须有独立进程 watchdog、超时 dump 和退出策略；不要让超时后的 std::async future 析构或无期限 join 再挂住测试。超时是失败，重跑不能抹去原结果。

模型工具入口与固定版本见 [tools/model/README.md](tools/model/README.md)。GenMC capability probe 只验证工具输入能力；项目原语模型另行建立。正确程序和故意错误程序都要验证，保留工具/LLVM版本、源码身份、输出、退出码及反例图。不得把原生运行、有限 SC 调度、generic+TSan 或入口文件存在当作弱内存/真实 futex 验证。

## 7. 性能实施与验收

- 先保留正确的 mutex 基线。每次改变一个主要机制，冻结 commit、binary、checksum 和历史数据，同日重跑 binary 作正式 A/B。
- 性能分析优先看算法/调度成本、allocation、data movement、cache coherence/false sharing、共享 RMW、停车扫描、硬件利用率及 NUMA；不得无 benchmark 声称无锁或某个内存序一定更快。
- 不宣称热路径零共享/零分配：pending、提交门、submitted、epoch 都有共享成本，Task 包装仍有分配。优化要给出收益与代价转移的归因。
- 固定机器、工具链、CPU集合、producer/worker数、任务工作量、拒绝策略和缓冲配置；预热、重复与测量窗口遵守 benchmark-plan。
- worker_completed、inline_completed、total 分开；CallerRuns 可超过 worker_count 的实际并发，不能用更多 producer 的内联工作制造 worker scaling。
- 同时报告吞吐和 p50/p99/p999、空载/关停 CPU、拒绝/丢弃、窃取和通知扫描。post_wake 探测包含虚假/中断返回，不能冒充内核实际唤醒线程数；generic 不伪造 wake_threads。
- 最终性能验收在固定物理 Linux 运行。macOS/WSL、x86_64、generic 后端不能替代要求的 Linux/ARM64 证据。
- 六项门槛、标定范围和五问以主设计/benchmark-plan 为唯一来源，不在这里复制易漂移数字。门槛变更附环境、理由和双侧数据，不能挑负载或只改文字。
- allocator、分片、NUMA、通知抑制、免 fence、helping future 和任务预算按数据及当前授权立项，不作预防性扩展。

## 8. 变更交付与工作区纪律

修改 API、ownership、调度、等待或错误语义时同步主设计、相关 ADR、测试和里程碑状态。说明具体 trigger、before/after、采用方案的原因、验证与未覆盖范围。

每个里程碑在 `results/milestones/Mx.md` 记录实际命令、源码 revision/dirty 状态、平台/后端、交付、回归、模型及性能结果。新运行用新目录，归档必要源码 hash、原始日志与反例；不覆盖旧失败或把本机成功记成远端 CI 成功。

只报告本轮真正执行或明确引用的证据；区分工程 smoke、模型能力探针、原语模型、正式池级测试及性能验收。剩余资源/门禁写明，不把进度或文件落盘当作完成判据。

保护现有未提交改动与用户文件，不使用破坏性清理恢复“干净目录”，不编辑生成头或外部依赖源码冒充项目修复。提交、推送、发布和系统工具安装按用户已授权范围及环境权限执行，记录开发工具安装带来的实际系统变化。日常开发优先复用已验证工具，不自动升级版本或重复安装。
