# magpie 实施步骤与里程碑

- 日期：2026-10-03。
- 状态：**M0 待 Linux CI；M1 mutex 底座已通过本机验证、待平台/测试收口；M2 工程与本机开发冻结/同日重跑已完成、待物理 Linux 完整验收；M3–M7 未开始**。
- 范围：实施当前审核修订版线程池，形成正确性证据、冻结基线和首版交付。M1 尚无 Linux pthread backend 原生验收；完整证据见 [M1 milestone](../docs/milestones/M1.md)。
- 上位约束：[主设计](magpie设计方案.md)、[逐项修复对照](review-fixes-2026-10-03.md)、[测试计划](test-plan.md)、[benchmark 计划](benchmark-plan.md)。

## 1. 实施目标与顺序

推荐先完成可独立交付的 mutex 基线，再依次替换队列、本地调度和停车机制。每次替换只改变一个主要机制，保留上一版 binary 和结果，使 correctness 回归及性能变化可以归因。

已有设计辅助验证的 15 项检查不等于库级验收。后续必须把相关交错迁移到真实原语和真实 ThreadPool；不能用辅助 fixture 替代实际调用路径。

关键路径：

```mermaid
flowchart LR
    M0["M0 工程骨架"] --> M1["M1 mutex 正确性底座"]
    M1 --> M2["M2 冻结基线"]
    M2 --> M3["M3 MPMC 接入"]
    M3 --> M4["M4 本地队列与窃取"]
    M4 --> M5["M5 EventCount 与退避"]
    M5 --> M6["M6 性能验收"]
    M6 --> M7["M7 首版交付"]
```

这些是交付依赖，不要求所有探索串行：模型工具验证、平台环境准备和独立原语研究可以提前进行，但集成里程碑必须按门禁推进。未通过的原语不能成为下一版默认运行路径。

## 2. 里程碑总览

| 里程碑 | 对应工作项 | 核心交付物 | 完成判据 |
|---|---|---|---|
| M0 工程骨架 | W1.1 | CMake、平台配置、测试/CI、模型验证入口 | 构建与测试基础设施可运行，验证工具能力明确 |
| M1 正确性底座 | W1.2–W1.6 的公共语义部分 | 可用 mutex 单队列 ThreadPool | API、所有权、关闭、拒绝、异常、执行帧、统计测试通过 |
| M2 基准冻结 | W1.7 | benchmark harness、冻结基线 commit/binary/数据 | 可复现同资源测量，基线归档可校验与重跑 |
| M3 全局 MPMC | W2.1、W2.2 | MPMC 原语及池内替换版 | 弱 try 契约、reservation hole、守恒通过，性能数据可归因 |
| M4 本地队列与窃取 | W1.6 的完整快路径、W2.3、W2.4 | Chase-Lev、owner TLS、单项窃取及 fallback | R1–R4 回归、模型检查、池级守恒与重入通过 |
| M5 EventCount | W2.5，W2.6 的接入部分 | 每 worker WaitSlot、Linux/generic 后端、三级退避 | R5/R6、真实 syscall、注册守恒及关闭测试通过 |
| M6 性能验收 | W2.6；按数据选取 W2.8–W2.10 | 固定配置、完整负载矩阵、性能报告 | 主设计六项门槛通过或完成有证据的门槛修订 |
| M7 首版交付 | 首版发布收口 | 可安装库、用户文档、示例、验证归档 | 正式测试与平台矩阵通过、交付可复现、限制明确 |

完成状态只认交付物和证据，不按“代码已写完”关闭里程碑。日历日期应在 M0 确认开发人力、Linux 物理机和工具环境后排定；本计划不把未知资源下的估时当作交付承诺。

## 3. M0：工程骨架与验证环境

当前进度（2026-10-03）：本机 macOS 三变体和静态/共享 consumer fast tests 通过；GenMC v0.17.0/LLVM 19.1.7 的 RC11 atomic/fence 正反探针通过。GitHub Actions 工作流已定义但尚未远端执行，故 M0 仍待 Linux CI 验收。完整记录见 [M0 证据](../docs/milestones/M0.md)。

**目标：**让后续每个模块都有可编译、可测试、可诊断的工程入口。

实施步骤：

1. 建立 C++20 CMake 工程，生产目标与 tests/bench 分离；队列和 deque 为 header-only，ThreadPool、线程后端、停车后端分别编译。
2. 建立 release、TSan、ASan+UBSan 三个独立构建目录。TSan 与 ASan 不混用，测试钩子默认关闭，生产运行路径无测试回调。
3. 固定 GTest release tag，配置 ctest 的 fast/stress/soak LABELS、独立进程超时和失败输出。
4. 落实配置校验、缓存行构建宏、导出宏和静态/共享库消费方式；应用最低工具链检查及 feature-test fallback。
5. 确认 Linux pthread 与 generic std::thread 后端边界、原生 Linux futex 测试环境和最终物理 Linux benchmark 环境。记录 ARM64 可用性，作为未来内存序放宽门禁的资源。
6. 选择并记录有界模型工具、版本和运行方式。先用最小 atomic/fence 用例验证其表达能力；仅能枚举 SC 操作顺序的脚本不能替代 C++ 弱内存模型检查。
7. 建立证据目录及报告模板，记录源码 revision、工具链、后端、测试配置、随机种子和交错轨迹。

交付物：

- 工程目录、CMake 配置、CI 定义、基础测试入口。
- 平台/工具链矩阵，模型工具能力和运行说明。
- README 的开发构建命令与归档规则（`results/` 原始归档不随仓库跟踪）。

退出门禁：空工程与最小库消费程序在声明支持的平台上构建通过；三变体可运行最小测试；工具和平台缺口显式登记。生产配置不能导出测试钩子。

资源缺口处理：Linux 物理机暂缺允许继续 M1；不能关闭依赖真实 syscall 的 M5 或性能验收 M6。GenMC 工具 capability probe 已通过；deque/MPMC/EventCount 的项目专用模型仍须在对应 M4/M5 集成验收前完成。

## 4. M1：mutex 正确性底座

当前实现进度（2026-10-03）：公共 API、mutex ring、拒绝策略、执行/owner scope、提交 Gate、pending/drain 握手、析构与构造回滚、worker/inline 统计以及 generic backend 已实现。最终本机结果为 Release fast 6/6、Release stress 1/1、TSan fast 5/5、ASan+UBSan fast 6/6；death suite 在 Release/ASan 独立进程运行且不进入 TSan。Linux `pthread_create`/属性源代码已实现但尚无 Linux 编译、CI 或原生运行结果。没有实现 M2 benchmark。完整命令和日志见 [M1 证据](../docs/milestones/M1.md)。

**目标：**先得到语义完整、可独立使用的线程池，后续优化沿用这些公共协议。

实施步骤：

1. 实现 Options 规范化、Task/TaskImpl、薄模板 submit/submit_async、QueueFullError 和 callable 约束。
2. 实现固定 worker 组及全局有界 mutex 队列；worker 的等待谓词与通知用同一 mutex 建立检查到等待的握手。
3. 实现提交 Gate、计数先行的 pending 和统一 dec_pending；拒绝/丢弃均正确销毁并回退，归零通知持 drain_mtx。
4. 实现 CallerRuns/Abort/DiscardOldest，定义丢弃 future 的 broken_promise。内部提交队列不可用时仍遵守 CallerRuns 完成义务。
5. 实现独立 owner TLS 与可嵌套 ExecutionFrame 链；为同步 shutdown/drain/析构接入 release 有效的执行上下文 guard。
6. 实现 shutdown/drain/join、构造分阶段初始化与部分启动失败回滚。Linux 使用 pthread_create(attr)，generic 后端遵守 stack_size 忽略语义。
7. 实现异常回调防逃逸、Task 销毁执行 scope，以及 worker/inline 分开的 Stats。
8. 配置 API 编译正反例、确定性提交门/归零通知缝、并发策略和生命周期测试。

此阶段没有 Chase-Lev 本地队列；W1.6 先完成身份识别和内部提交语义，真实 deque 快路径在 M4 接入。验证报告须明确当前调度结构，不声称本地快路径已完成。

关键验收用例：SubmitAndDrain、FutureException、RejectionPolicies、DiscardedFutureBrokenPromise、ShutdownGateRace、DrainNotifyRace、RejectionRollbackNotifies、CallerRunsControlGuard、NestedExecutionControlGuard、CapturedDestructorControlGuard、ConstructorFailureRollback、OptionsValidation、StatsQuiescentIdentity，以及 API 编译用例。

退出门禁：

- 对应 fast/stress 测试在适用 sanitizer 变体通过；death 用例按测试计划隔离。
- 外部 CallerRuns 和回调中的非法生命周期调用按契约失败，不能挂死。
- 部分 worker 已等待时注入线程创建失败，全部已建线程被唤醒并 join，原异常被重抛。
- 停止生产并排空后，每个接受 id 恰执行或丢弃一次，pending=0，未饱和统计满足 submitted=completed+discarded。

交付：可用 mutex 版本、完整公共 API 测试、构造与生命周期诊断记录。高性能目标不是此阶段的退出条件。

## 5. M2：benchmark harness 与基线冻结

当前进度（2026-10-03）：opt-in `magpie_bench`、warmup/固定窗口、预分配记录与分组直方图、CPU 秒、资源集合、独立进程重复/median/IQR、源码快照 commit/bundle、binary 校验和同日 replay 已实现。当前 M1 适用程序全部可运行；无本地队列的 worker scaling/skew 与无 EventCount 的 notify scan 不适用。macOS smoke/开发数据不关闭物理 Linux 完整矩阵门禁，逐项证据见 [M2 milestone](../docs/milestones/M2.md)。

本机最终交付：三变体各 30 组 smoke、fast Release 8/8、TSan 7/7、ASan+UBSan 8/8；冻结源码 fresh Release fast/stress 通过。代表空任务配置完成初次 5 进程和原 binary 同日重跑 5 进程（每次 2s warmup/10s duration），源码 bundle 恢复与全部 hash 校核通过。完整长窗口矩阵和正式冻结尚待物理 Linux；本机重跑噪声不能忽略，不作性能达标结论。

**目标：**在替换任何核心机制之前，取得可信的性能对照。

实施步骤：

1. 建立 warmup、固定测量窗口、重复进程、固定资源集合、到达速率和 TSV 输出。
2. 区分外部提交、worker 执行、本地工作产生、CallerRuns 完成和纯队列成本；支持时延直方图与 CPU·秒。
3. 实现 benchmark 计划中的基础程序；在 M1 结构上适用的程序先完整采集，尚依赖本地队列的维度注明不适用。
4. 验证测量过程本身不引入共享热计数器/每任务分配，不把 future 等待变成隐式 helping。
5. 冻结 API 语义匹配的 mutex 基线 commit、binary、sha256 和原始数据。
6. 当天重跑冻结 binary，确认归档可用、配置一致，报告重复性及噪声。

交付：harness、基线报告、可校验 binary 和原始 TSV。若基线仍有正确性失败，修复后重新形成首次有效冻结点。

退出门禁：固定配置至少 5 次独立运行，预热≥2s、测量≥10s；记录中位数/IQR、线程/CPU预算、拒绝策略和 buffer 配置。正式性能结论来自物理 Linux。

后续比较注意：mutex 单队列和本地队列版本的总缓冲量可能不同，必须同时记录 global/local/总槽数与 inline 比例。不能将更大缓存或更多 producer 执行资源的收益误归因于原语本身。

## 6. M3：全局 MPMC 原语与池内替换

**目标：**只替换全局队列，保持公共协议及停车机制不变。

实施步骤：

1. 实现 MPMCQueue<T> 存 T，池实例化 Task*；认领后的数据读写不得抛异常。
2. 实现 seq 初始化、position CAS、发布/释放协议、容量/byte size 校验和索引上限处理。
3. 注入 producer/consumer position 认领后的确定性暂停缝。
4. 建立弱 try 历史 oracle；不可用返回不等于严格空/满，成功 position 和 id 仍需守恒。
5. 原语验收通过后接入真实 ThreadPool，复跑 M1 的提交、拒绝、异常、关闭、计数及丢弃用例。
6. 采集纯队列和完整池的 producer 扩展性，同日比较 mutex 基线；解释 gate/pending/epoch/alloc 等剩余共享成本。

关键用例：SingleThreadFifo、WrapAround、MultiProdMultiCons、ProducerClaimStall、ConsumerClaimStall、RelaxedQueueHistory、MpmcIndexLimit、DiscardUnderLoad。

退出门禁：原语和池级适用测试通过，reservation hole 恢复后所有任务可消费，失败 try 不泄漏位置/Task。性能报告必须能解释变化；如果优化目标未实现，保留上一版作为默认，不为里程碑进度强行替换。

交付：经过测试的 MPMC 原语、全局替换版及五问报告。弱 try 契约与相应尾延迟边界进入用户文档。

## 7. M4：本地 deque、工作窃取和池层 fallback

**目标：**接入审核修订后的单元素协议，恢复本地调度能力。

实施步骤：

1. 实现 bounded Chase-Lev try_push/pop/steal，完整保留双侧 SC fence；deque 不引用全局队列、EventCount 或统计。
2. 实现 R1/R2 的真实确定性交错，并对小状态、slot 复用、失败候选和索引上限做模型检查。
3. 先构造全部 WorkerCtx，再启动线程；owner TLS 同时核对池身份，地址保持稳定。
4. 接入本地→全局→窃取顺序。池层重复单项 steal，接收量钳制 local_capacity；转移批全部完成前不得执行用户代码。
5. 本地满只转投当前新任务，两个队列均失败才 CallerRuns；不恢复旧半批 spill/overflow。
6. 验证跨池提交、16/1024 容量边界、重入、关闭拒绝子项以及异常路径计数。
7. 采集本地 spawn、偏斜、worker scaling 和 allocation 成本，区分 inline_completed。

关键用例：StealAcrossOwnerPops、LastElementCasFailureRestoresBottom、StealFromFullDequeDeterministic、LocalOverflowDefaultCapacity、LocalOverflowTaskConservation、BulkReceiveCapacityBound、CallerRunsReentrancy、CrossPoolSubmit、FullScanCoversAllVictims。

退出门禁：

- 原语模型检查和对应正式回归通过，状态快照断言只在受控稳定边界进行。
- 接入真实池后有限子任务负载不丢失、不重复，完整关闭后 pending=0。
- 队列内部没有用户代码执行；嵌套 CallerRuns 的 Task/执行帧恢复正确。
- 保守内存序版本获得同日性能数据；不把内存序放宽当作进入 M5 的前置条件。

交付：单项 deque、完整 owner 快路径、工作窃取版和偏斜报告。旧批量 CAS 的证明与实现不得复用。

## 8. M5：每 worker WaitSlot EventCount 与三级退避

**目标：**使 Linux/generic 后端在真实池中满足注册、通知和关闭握手。

实施步骤：

1. 实现稳定分配的 WaitSlot、共享 epoch/注册数、Idle/Waiting/Signaled 状态及 round-robin 通知扫描。
2. 实现 prepare→enter→stop 检查→只认领的最后扫描→epoch 复查→阻塞→leave；所有路径配平注册，注销前不运行用户代码。
3. 实现 generic 每槽 mutex/cv 与 Linux per-slot futex 后端，检查 Linux atomic word ABI、PRIVATE 操作、EAGAIN/EINTR 和其他错误策略。
4. 做注册/注销/重注册、并发 notify、关闭及 spurious 的有界状态与弱内存检查；至少覆盖两轮注册生命周期。
5. 在 Linux 原生强制 syscall 前后的测试缝，复现 R5/R6 原有调度。generic 通过不能替代这一步。
6. 替换真实池的等待机制，接入 spin/yield；统一所有成功发布点的通知义务。
7. 完整复跑之前里程碑的池级测试，采集 epoch、扫描长度、wake 次数、空载 CPU 和 p99。

关键用例：SeparateWaitWords、WakeBeforeKernelWait、ShutdownBeforeRegistration、ShutdownAfterRegisteredCheck、ConcurrentNotifyRegistration、RegistrationBalanced、EpochLimit、SpuriousWake、GenericPredicateHandshake、SpawnWhileIdleWakeup、DrainDuringShutdown。

退出门禁：Linux 与 generic 语义测试均通过，Linux 真实窗口和模型结果归档；停止后的晚注册不能睡过最终通知；全部 join 后注册数归零。统计区分 syscall 返回和 backend 返回后的探测。

交付：完整 EventCount、退避接入版、平台测试与初步性能归因。若注册/关闭证据失败，保持上一版停车机制，不合入轻量通知优化。

## 9. M6：性能标定与正式验收

**目标：**判断当前架构是否达到项目目标，而非证明某个原语“理论上更快”。

实施步骤：

1. 固定测试机器/资源预算/工具链，完整运行任务粒度、producer/worker数、偏斜、突发、非阻塞嵌套和长任务关闭矩阵。
2. 标定 BULK_LIMIT、STEAL_CAP、SPIN_LIMIT、YIELD_LIMIT、VICTIM_TRIES，每轮只改变可解释的变量，保持入睡前全 victim 扫描。
3. 分别报告 worker、inline、total 吞吐，分析 allocation、global CAS、gate/pending/submitted/epoch 和通知扫描。
4. 按数据决定是否启动槽布局对照、关停停车或通知扫描优化；修改协议须回到对应模型与回归门禁。
5. 同日重跑冻结 mutex binary，输出五问、原始数据和误差分布。
6. 对照主设计 §14.1 当前六项数值门槛逐一判定。

| 项目 | 当前验收初值 |
|---|---|
| 单 worker 空任务 | ≥1.0×10⁶ worker completions/s，无 CallerRuns 混算 |
| 独立 worker scaling | T8/T1≥6.4，固定 driver、供给充足、inline 单列 |
| 单热点退化 | 同资源吞吐损失≤20% |
| p99 | ≤同日重跑基线的1.05×，无新峰 |
| 空载 CPU | ≤3%/worker，10s窗口 |
| 对比提升 | 显式 launch::async ≥2×；每任务一线程 ≥10× |

退出门禁：六项都有有效结论；未达到时分析机制并修复，或按主设计修订通道提交硬件/双侧数据和理由后调整门槛。只改文字、挑选负载、放宽资源预算不构成通过。

交付：当前生效常量、完整验收报告和明确性能适用范围。**内存序放宽不是首版必需交付**；若开展 W2.7，每处都必须单独满足书面模型、模型检查、ARM64、sanitizer和同日数据条件。

## 10. M7：首版交付与发布准备

**目标：**形成用户能够构建、消费、诊断和正确使用的版本。

实施步骤：

1. 用冻结的候选 revision 跑正式 fast/stress 与小时级 soak，记录全部结果；修复后重测受影响模块与集成路径。
2. 验证声明支持的平台/工具链、静态/共享库消费、安装路径、导出符号以及生产构建没有测试钩子。
3. 补齐 README/API 示例：外部 submit、future 异常、拒绝恢复、shutdown/drain、统计读取；示例不在本池任务里阻塞等待本池 future。
4. 说明 CallerRuns 的内联/并发/栈深语义、生命周期禁令、对象外部 lifetime、弱 try、drain 观察点和 ABI 不承诺。
5. 沿用现有 Apache-2.0 LICENSE，归档候选 commit、构建配置、测试/模型/benchmark证据及已知限制。
6. 形成首版交付评审记录和发布说明；实际发布动作按届时授权执行。

退出门禁：没有未解释的 correctness、所有权、并发或关闭失败；支持矩阵与文档一致；性能验收可追溯；用户示例编译运行通过；已知限制均已明确，不以“TSan 全绿”宣称形式正确性。

交付：可安装的首版候选、发布说明、用户文档及完整证据包。M7 完成不表示已对外发布。

## 11. 工作项覆盖与条件性演进

| 工作项 | 落位 |
|---|---|
| W1.1 | M0 |
| W1.2 Task/薄模板/future | M1；API 示例和消费验证在 M7 收口 |
| W1.3 提交门/pending/策略 | M1；所有后续版本复跑对应回归 |
| W1.4 生命周期与构造回滚 | M1；M5 复核新停车后端的关闭握手 |
| W1.5 Stats | M1；M6 校验性能统计与归因口径 |
| W1.6 | 身份/内部语义在 M1，完整本地快路径在 M4 |
| W1.7 | M2 |
| W2.1/W2.2 | M3 |
| W2.3/W2.4 | M4 |
| W2.5 | M5 |
| W2.6 | 接入在 M5，完整标定/性能验收在 M6 |
| W2.7 | M6 后可选；未经放宽的保守版本可进入 M7 |
| W2.8/W2.9/W2.10 | M6 按明确数据触发，不是预防性重构任务 |
| W2.11 | 首版后独立语义草案，不阻塞本计划 |
| W3.x | allocator、NUMA/分片、预算、快照屏障等独立立项 |

本计划不引入一次 CAS 半批 steal、旧 overflow 暂存、共享旧 epoch 睡眠字或默认 helping wait。新需求改变 API/调度语义时先更新主设计和 ADR，再新增里程碑。

## 12. 交付与证据管理

每个里程碑建立 `docs/milestones/Mx.md`，包含：

1. 状态、目标、实际源码 revision 和相对上一版的机制变更。
2. 交付文件、公开 API 影响及对应 W/R 项目。
3. 实际命令、工具链/平台/后端、测试结果、seed及确定性交错轨迹。
4. 模型输入与覆盖边界，尚未验证的场景。
5. benchmark原始数据、资源/语义一致性及五问结论（适用时）。
6. 缺陷、回退决定、下一里程碑是否已满足前置。

原始数据、冻结 binary 和校验和不可覆盖；重跑结果用新文件。CI job失败需关联具体协议或代码问题，不能仅以“偶发”关闭。

状态流为“未开始→进行中→待验收→完成”；资源缺口标记在当前里程碑报告，不因期限接近自动关闭。任何 correctness 回归优先恢复上一版，再定位修复。进度按门禁和有效交付量报告。

## 13. 当前可执行的下一步

当前可执行下一步是为 M1 运行 Linux CI，并复核 pthread stack/affinity fallback。该平台门禁通过后再进入 M2 冻结 mutex 基线；当前没有必要扩充 NUMA、任务图或自定义 future。实施过程中以这份计划组织交付，以主设计和三个 ADR 决定协议，以测试和性能证据判定是否进入下一里程碑。
