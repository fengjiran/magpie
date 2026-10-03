# test-plan：单元、确定性交错与池级验证

- 状态：**2026-10-03 审核修订版，正式实现与门禁待执行**。
- 上位：[magpie设计方案.md](magpie设计方案.md) §10；ADR-001/002/003。
- R1–R7 回归矩阵替代旧半批/overflow/共享字用例；旧反例保留在审核报告。

## 1. 边界

正确性测试只断言所有权、计数、契约、合法状态与活性。watchdog 是挂死兜底，不能当性能阈值。benchmark 不证明正确性。

design/validation/check_design.py 是无需库实现的辅助检查：提取文档骨架、编译、运行有限回归；不能替代库级线程池、Linux futex 或 ARM64 验收。报告必须区分编译检查、状态机调度、真实多线程和正式池实现。

## 2. 框架

正式库测试采用 GTest，CMake FetchContent 固定 release tag（沿用 v1.14.0 初值，升级单独记录），不跟 master。按模块拆二进制；backend 用 TEST_P。DeathTest 使用独立进程，TSan 不跑 fork/death。低影响文档修订不引入生产第三方依赖。

生命周期禁令 shutdown/drain 抛 logic_error，可正常断言；本池执行帧内析构 terminate 必须单独 death 测试，不能破坏测试 runner。

## 3. 分层与 CI

| 层 | 内容 | 触发 |
|---|---|---|
| T1 fast | 顺序、确定性缝、配置/API、有限状态 | pre-merge，release/TSan/ASan+UBSan |
| T2 stress | 百万次随机及竞争（后缀 Stress） | nightly；TSan缩放写报告 |
| T3 soak | 小时级压力（后缀 Soak） | weekend，ASan+UBSan |

ctest fast/stress/soak LABELS；外层超时 300/3600/86400s。watchdog 不用阻塞式 std::async future 析构收尾，否则超时后仍会挂死；采用独立测试子进程或能 dump 后退出的守护。ASan/UBSan 与 TSan 分开。

## 4. oracle 与模型

### 4.1 参照规格

DequeModel：有界 std::deque，try_push 满则不接收，pop 尾项，steal 首项。并发 steal CAS 竞争可返回 unavailable；记录输入、调用/返回、返回 id、成功认领位置和测试缝轨迹，不复刻旧半批钳制。

MPMC 弱 try 模型允许 enqueue/dequeue unavailable。成功认领的 position 定义顺序，已发布后继可与头部 unavailable 并存；consumer 未释放可阻止槽复用。严格 std::queue 只用于无并发暂停的单线程顺序测试。

### 4.2 判定

- 守恒：所有接受 id 在 executed/discarded 中恰出现一次，拒绝 id 不被消费；测试结束清空剩余任务。
- 顺序：仅在满足前提的受控场景断言 LIFO/FIFO；不把完成顺序当认领顺序。
- deque 稳定边界：owner 操作已完成并在测试门控下取得一致快照时验证 0≤bottom−top≤capacity。pop 中间态 bottom<top 合法，任意分离 load 快照不能强制断言。
- MPMC：slot 世代和成功 position 关系；不能用“已返回 enqueue 数−已返回 dequeue 数”当瞬时真实占用。
- pool：停止且完全 quiescent 后 submitted=completed+discarded、pending=0；rejected 含 CallerRuns 不减入恒等式。
- future：正常/异常/DiscardOldest broken_promise 分别断言。

多重集合正确不证明每个返回合法；历史检查与原子模型检查不可省略。

### 4.3 随机与 replay

固定种子 {0x9E3779B9,0x517CC1B7,20260929}，MAGPIE_TEST_SEED 覆盖。owner push/pop，2~3 thief；MPMC 2~4 producer/consumer。任务 id 生命周期与调用轨迹完整归档。

相同 seed 只复现 op 序列，不复现 OS 调度。确定性 replay 必须记录/驱动测试缝暂停与释放顺序，不能仅按 seed 逐项重跑。

### 4.4 证据层次

L1 随机守恒 + L2 确定性缝 + L3 小历史/弱内存有界模型。模型覆盖有限状态，不称全规模证明；TSan 捕捉数据竞争，不证明 atomics/lost wake。正式测试与文档辅助状态机各自归档结果。

### 4.5 测试缝唯一注册表

| 结构 | 暂停点 | 测试 |
|---|---|---|
| deque steal | 候选读取完成、CAS前 | StealAcrossOwnerPops / StealFromFullDequeDeterministic |
| deque pop | 最后项expected已读、CAS前 | LastElementCasFailureRestoresBottom |
| MPMC enqueue | position CAS后、seq发布前 | ProducerClaimStall |
| MPMC dequeue | position CAS后、seq释放前 | ConsumerClaimStall |
| block_on_eventcount | 可选前置stop检查后、prepare前 | ShutdownBeforeRegistration |
| EventCount enter | waiters++后、Waiting写前 | ConcurrentNotifyRegistration |
| registered stop | stop检查false后 | ShutdownAfterRegisteredCheck |
| EventCount wait | epoch复查后、backend_wait前 | WakeBeforeKernelWait / SeparateWaitWords |
| backend_signal | CAS标记后、wake前 | WakeBeforeKernelWait / ConcurrentNotifyRegistration |
| submit | 过门后、pending前 | ShutdownGateRace |
| dec_pending | 1→0后、drain锁前 | DrainNotifyRace |

release 默认剔除。新增缝必须登记测试，不用于生产回调。旧 deque spill 缝删除，不能继续测试已不存在的协议。

### 4.6 有界模型检查

deque：2~3线程、容量4，完整双側 SC fence；关注普通pop、最后项、slot复用、失败候选。弱 try 不要求每次CAS失败都成功取项，但成功id必须正确。

EventCount：N=1/2/3，每worker最多一次注册，多个通知者，epoch/注册/Waiting/Signaled/Idle全协议；包含stop、复查后暂停、注销/重注册、EAGAIN/spurious、generic mutex与Linux比较等待各自模型。至少枚举两次注册生命周期，防旧notify落到新注册的盲区。有限 epoch 测试加入上限fail-fast，不用回绕等价简化而掩盖ABA。

MPMC：单独验证成功位置守恒与弱try历史；不能挂严格FIFO LinearizabilitySmoke 后用宽松解释消除报错。

## 5. 活性与失败现场

T1 watchdog 默认10s，T2 120s，T3由外层job限制；可按工具链调整并登记，不用benchmark结果自动改变时限。超时dump：pending/stopping/gate、队列position/seq、epoch/注册数/每槽状态、ExecutionFrame及线程堆栈，再终止子进程；不无限等待join。

所有 stop/drain、notification、构造回滚用例挂watchdog。多次重跑不能抹掉一次超时失败。

## 6. 正式用例清单（名称唯一来源）

| 文件 | 用例 |
|---|---|
| deque_test.cpp | PushPopSequential、TwoThreadPushSteal、LastElementRaceStress、StealAcrossOwnerPops、LastElementCasFailureRestoresBottom、StealFromFullDequeDeterministic、StealFromFullDequeStress、DifferentialRandomOps、LinearizabilitySmoke、DequeIndexLimit |
| mpmc_queue_test.cpp | SingleThreadFifo、WrapAround、MultiProdMultiCons、ProducerClaimStall、ConsumerClaimStall、RelaxedQueueHistory、MpmcIndexLimit |
| event_count_test.cpp | SeparateWaitWords、WakeBeforeKernelWait、ShutdownBeforeRegistration、ShutdownAfterRegisteredCheck、ConcurrentNotifyRegistration、RegistrationBalanced、EpochLimit、SpuriousWake、GenericPredicateHandshake |
| pool_test.cpp | SubmitAndDrain、FutureException、RejectionPolicies、ShutdownGateRace、ShutdownRace、DiscardUnderLoad、DiscardedFutureBrokenPromise、DrainDuringSubmit、DrainDuringShutdown、DrainNotifyRace、WorkerSubmitsChild、SpawnWhileIdleWakeup、FullScanCoversAllVictims、CrossPoolSubmit、RejectionRollbackNotifies、BurstAcrossShutdown、DiscardRetryExhausted、HandlerThrowsSwallowed、LocalOverflowDefaultCapacity、LocalOverflowTaskConservation、BulkReceiveCapacityBound、CallerRunsReentrancy、CallerRunsControlGuard、NestedExecutionControlGuard、WorkerControlGuard、DestructorControlGuardDeath、CapturedDestructorControlGuard、ConstructorFailureRollback、OptionsValidation、StatsQuiescentIdentity、SoakLongRun |
| api_compile_test.cpp | LvalueOnlyCallableAccepted、RvalueOnlyCallableRejected、MoveOnlyCallable、NonconstructibleLvalueRejected、FutureResultValueCategory |

R1/R2必须精确强制审核反例顺序。R3/R4在本地容量16/1024、全局滿、有限子任务递归下验证旧项仍保留、新项执行和pending守恒，不再依赖overflow缓冲。

R5：A通过W5后暂停，通知标记A，B恢复注册；B不能改A的word，放行A后比较失败。R6两侧：shutdown在prepare前已完成、或发生在注册stop检查后；均不得晚睡。

R7：外部CallerRuns、worker局部满fallback、异常回调、capture析构、跨池嵌套回到较早执行帧，分别验证guard。捕获析构中调用控制API的测试应捕获logic_error；throw逃出noexcept destructor按既定语言规则terminate。

构造回滚在至少一个worker已停车后强制下一次创建失败；通知并join后重抛原异常。所有victim已构造的阶段约束须白盒验证。

## 7. 后端同测与平台

generic/futex同套语义用例；Linux原生真实syscall前后窗口为独立必需结果，不能用generic+TSan替代。Linux每槽4字节ABI、EAGAIN/EINTR错误路径测试。运行backend和实际wake_threads可用性写日志。

macOS上的辅助检查只验证generic与内核比较状态模型，不能标记Linux测试通过。

## 8. sanitizer与ARM64

release / TSan / ASan+UBSan分开。TSan不做DeathTest，压力缩放注明迭代数；LSan用于Linux泄漏。模型检查独立于sanitizer。

W2.7内存序放宽要求ARM64物理机或云机实测，并保留工具链/日志；资源不可得就不合入该放宽。x86上成功不能替代弱内存证据。

## 9. 编写纪律

每TEST独立池/队列，无跨测试可变全局。随机只经公共generator；失败带seed和交错轨迹。确定性测试允许断言明确驱动的交错；其他并发测试不假定OS执行顺序。

所有权、用户代码进入点和注册/注销在测试中单独追踪，不能为方便oracle复制有缺陷算法。生命周期违规是契约测试，正常负载不得依赖异常逃生。

## 10. 门禁

工作项对应正式用例×工具变体全绿、模型输入/结果归档，才可关闭。M0 已有 build-info/静态共享 consumer 基础用例，GenMC atomic/fence capability probe 也已归档；这不覆盖 ThreadPool 协议。W1/W2 的线程池和原语用例仍待实现。benchmark 门槛、Linux、ARM64 或 soak 缺证据必须明确列为待验证，不以文档变更冒充完成。
