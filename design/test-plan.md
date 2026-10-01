# test-plan：单元与集成测试方案（GTest）

- 状态：**已接受**（数据集随实现推进回填）
- 日期：2026-09-29
- 修订：P3-p（2026-09-29）：§6 补 StealFromFullDequeStress/SpillOverflowReentrancy；§5 活性清单补 SpillOverflowReentrancy
- 修订：P3-u（2026-09-29）：§5/§6 补 SpawnWhileIdleWakeup（场景 #19）
- 修订：P3-w（2026-10-01）：§6 更名 BulkTransferPreservesFifoOrder（取代 GlobalFifoEndToEnd）
- 修订：P3-x（2026-10-01）：§6 补 ProducerClaimStall（MAGPIE_TEST_HOOK 缝）
- 修订：P3-y（2026-10-01）：**oracle 能力边界 + 三层证据金字塔**——§4.2.1 声明差分 oracle 不构成线性化验证；§4.5 确定性 barrier 交错层（缝注入点全集，补 #17/#18 确定性变体）；§4.6 小状态空间线性化检查器（common 组件 + 两个 LinearizabilitySmoke）；§6 落位回写
- 修订：P3-ab（2026-10-01）：**参照模型契约对齐**——§4.1 DequeModel 的 steal(k) 补 `k==0 ⟹ 头删 0`（消除与实现"同错同过"）；§6 deque 清单补 StealZeroContract
- 修订：P3-ad（2026-10-01）：**P0 修复与口径修正的测试侧联动**——§4.5 steal 缝位改"候选锁存完成、CAS 认领之前"（旧缝位对应已废止读序）；§5/§6 补 `CrossPoolSubmit`（#20）与 `RejectionRollbackNotifies`（#21），`SpillOverflowReentrancy` 增补"容量 16 × BULK_LIMIT 32 连续 bulk"变体（守护 P0-1）
- 上位约束：[magpie设计方案.md](magpie设计方案.md) §10.4（验证矩阵与测试清单）、§16.1（构建与 CI）、§16.4（工作项门禁）；ADR-001/002/003 的"测试对应"节；与 [benchmark-plan.md](benchmark-plan.md) 的边界见 §1。

---

## 1. 定位与边界

本文档将主设计 §10.4 的**名称级测试清单**落地为**可执行工程方案**：框架选型、分层运行策略、差分测试 oracle 规格、活性测试的 watchdog 纪律、sanitizer/平台映射与编写规范。

与 benchmark-plan 的边界（纪律，不可混淆）：

- **测试只断言正确性**：守恒、不变量、语义、活性（有界完成）；**禁止 wall-clock 性能断言**（如"p99 < X"），性能结论一律走 benchmark-plan——性能断言是 flaky 的最大来源；
- 测试允许的"时间"只有一种用法：活性 watchdog 的**上限**（§5），且不把它当性能结论；
- benchmark 数据不作为正确性门禁，测试通过也不代表性能验收（§14.1）。

## 2. 框架选型决策：GTest

| 项 | 决策 |
|---|---|
| 单元/集成测试框架 | **GoogleTest（GTest）**，v1.14+，经 CMake FetchContent 引入（锁定 release tag，不跟随 master） |
| 自研 harness | 放弃（本轮唯一决策变更点：主设计 §16.1 原写"二选一、立项时落 ADR"，本次落定） |

理由：

1. 本项目的测试主题是**多线程并发正确性**，测框架本身不是需要创新的对象——自研 harness 需要重造断言、报告、过滤、XML 输出、CI 集成，纯成本；
2. GTest 的两类参数化原语恰是本项目两个刚需的直接解：
   - `TEST_P`/`TYPED_TEST` 承载**差分测试的 op 序列参数化**（§4）与 **EventCount 双实现同测**（§7，backend 参数化）；
   - `ASSERT_DEATH` 承载 debug 构建的 assert 守卫验证（如 worker 内调用 `drain()` 的拦截，主设计 §9.5）；
3. GTest 在 TSan/ASan 下有成熟的使用约束与官方已知问题清单，sanitizer 变体的坑位是社区已探明的，而非本项目新踩；
4. XML 报告 + ctest `LABELS` 直接挂 CI 分层（§3）。

已识别的成本与对策（写入门禁，不是放弃理由）：

- GTest **无内建 per-test 超时**——活性类测试需要自定义 watchdog（§5）；且 ctest 层兜底 `--timeout`（§3）。此缺口对所有 C++ 测试框架同构，不构成选型差异；
- TSan 与 `fork` 冲突 ⟹ DeathTest 在 TSan 变体禁用（§8.1）；
- 单个测试二进制体积/构建时长上升——可接受，测试二进制按模块拆分（§6）控制增量编译面。

## 3. 测试分层与运行策略

所有测试用例按规模分为三层，以 **GTest 名称后缀约定**（`*Stress` / `*Soak`）区分，ctest 按 `LABELS` 分层执行：

| 层 | 后缀 | 单测规模 | 运行变体 | 触发时机 |
|---|---|---|---|---|
| T1 快速 | 无后缀 | < 2s/test | release + TSan + ASan/UBSan | 每次 pre-merge CI（三层各跑一遍全量 T1） |
| T2 压测 | `*Stress` | 百万次迭代、数秒～数分钟 | release + TSan（迭代经 `MAGPIE_TEST_TSAN_SCALE` 缩放，§8.1） | nightly job + pre-merge 可选触发 |
| T3 soak | `*Soak` | 小时级常跑 | ASan+UBSan | weekend 定时 job，失败即报警不阻塞合并 |

CI 集成约定：

- ctest 注册三层 LABELS：`fast` / `stress` / `soak`；
- 三个 CI job：`ci-fast`（三 sanitizer 变体 × 全量 T1，pre-merge 门禁）、`nightly-stress`（T2）、`weekly-soak`（T3）；
- **未通过对应层测试，对应工作项门禁不关闭**（§10 映射规则）；
- ctest 层兜底超时：CI 统一 `--timeout 300`（T2/T3 job 单独放宽到 3600/86400），防止 watchdog 失效时进程永久占用 runner——watchdog 提供**有现场的快失败**，ctest 超时是最后兜底。

## 4. 差分测试 oracle 规格（正确性底座）

无锁结构（`ChaseLevDeque` / `MPMCQueue`）的正确性验证手段："多线程随机 op 序列 × 并发执行"与"单线程参照模型"差分。规格如下。

### 4.1 参照模型

- `DequeModel`：单线程 `std::deque<Task*>` 模拟，操作语义与真实实现**逐条同构**：
  - push → 尾插；pop → 尾删（LIFO）；steal(k) → `k == 0` 时头删 0（P3-ab：与实现契约对齐，消除"同错同过"）；否则头删 `min(k, STEAL_CAP, size/2)`（不足 1 抬到 1）；spill → 头删 `size/2` 并记入"溢出集合"；
  - 全满时 push 触发 spill 的守卫逻辑同样复刻（守卫以 `size >= capacity` 判定）。
- `MpmcModel`：单线程 `std::queue` 模拟 enqueue/dequeue/discard_oldest；容量守卫同构。
- 模型必须与实现的**钳制规则**（STEAL_CAP、半批）同步演进——二者的行为差异清单任何一处不一致都被视为测试缺陷，模型代码注释指向主设计对应小节。

### 4.2 判定规则

| 判定 | 适用 | 内容 |
|---|---|---|
| 守恒判定 | 全部并发场景 | 任务带唯一 id（原子自增分配）：实现与模型各自记录的"消费多重集合"相等；每个 id 恰出现一次、不多不少 |
| 顺序判定 | 仅严格单生产者/单消费者场景 | 在守恒基础上额外比对消费**顺序**（如 `SingleThreadFifo`、单线程 LIFO 语义） |
| 非顺序判定纪律 | 其余并发场景 | **只比多重集合、不比顺序**——并发实现存在多个合法串行化，顺序比对会产生假阳性 |
| 不变量判定 | 每次差分全程 | deque：`0 ≤ bottom − top ≤ capacity`（主设计 §7.1.2 I1）；MPMC：`enqueue数 − dequeue数 ∈ [0, capacity]` 且槽世代无错位 |

### 4.2.1 oracle 能力边界（P3-y 补注：本 oracle 不构成线性化验证）

差分 oracle 的判定能力**精确等于**上表四行，不更多：

1. 守恒判定 ⟹ 发现**丢任务 / 重复任务 / 计数失衡**——强项，也是设计它的初衷；
2. 顺序判定 ⟹ 仅覆盖严格单生产者/单消费者路径；
3. **非顺序判定纪律 ⟹ 明确放弃 per-op 顺序比对**，因此以下违规**会静默通过**：错误 `empty` 返回（未消费，多重集合不变）、线性化点时点错误（消费集合仍等幂）、不满足实时序约束的"幽灵错序"历史——多重集合无法察觉；
4. 不变量判定 ⟹ 结构不变式，不涉及返回值的线性化论证。

即：**"逐对等价"仅指守恒 + 单线程顺序，不构成 linearizability 证明**。补齐手段是三层证据金字塔（§4.5/§4.6），任何修订不得以"差分测试全绿"表述"已验证线性化"。

### 4.3 op 序列生成器

- RNG：`std::mt19937_64`；默认种子集 = `{0x9E3779B9, 0x517CC1B7, 20260929}` 三个固定种子；环境变量 `MAGPIE_TEST_SEED` 可覆盖；
- 动作权重（deque）：`{push: 4, pop: 3, steal: 2, spill: 1}`；MPMC：`{enqueue: 4, dequeue: 3, bulk: 2, discard: 1}`；
- 容量边界施压：20% 概率在"满/空"边界触发对应操作（如 size==capacity 时 push、size==0 时 pop/steal），保证溢出守卫与判定路径被高频命中；
- 线程布局：deque 差分固定 `1 owner + 2~3 窃取者`；MPMC 差分固定 `2~4 生产者 + 2~4 消费者`；op 总数 ≥ 10⁶（T2 档）。
- **失败现场纪律**：失败输出必须包含 seed、完整 op log（含每 op 的任务 id 与结果）、双方消费多重集合的差异集——实现 **replay 模式**（同 seed 逐 op 重放）供复现，缺一个都算测试缺陷。

### 4.4 覆盖的测试

`DifferentialRandomOps`（deque）与 `MultiProdMultiCons` 加强版（MPMC 加差分）、`BulkStealInterleave`（窃取与 pop 全交错）、`SpillVsSteal`（溢出与并发窃取）——对应主设计 §10.4 清单与 ADR-001 §7 的性质列。

### 4.5 确定性 barrier 交错层（P3-y 新增，L2）

把"调度器才可能制造的暂停"变成确定性交错——`MAGPIE_TEST_HOOK` 测试缝（契约见 mpmc-queue-adr §6，P3-x 引入，P3-y 扩展）允许的注入点全集：

| 结构 | 注入点 | post-claim 语义 | 注册使用者 |
|---|---|---|---|
| `MPMCQueue::enqueue` | CAS 抢占 `enqueue_pos` 成功、`seq` 发布前 | 头部 reservation hole | `ProducerClaimStall`（P3-x） |
| `ChaseLevDeque::steal` | **候选锁存完成、CAS 认领之前**（P3-ad 缝位修正：旧"CAS 成功后、批锁存前"对应的读序已被 P3-r 废止、该时刻不存在） | top 未推进、push 守卫阻断覆盖；释放后 CAS 成功或失败均合法 | `StealFromFullDequeDeterministic`（场景 #17） |
| `ChaseLevDeque::spill` | top CAS 认领成功后、逐槽转移前 | 认领区间悬置、overflow 通道待收口 | `SpillOverflowReentrancyDeterministic`（场景 #18） |

纪律：每个注入点必须有一个 registered 测试；release 构建编译期剔除（默认空实现）；不得用于测试以外的任何用途；新增注入点须先更新本表 + ADR-003 §6 契约。**确定性变体与应力版共存**：应力版（`*Stress`）负责统计学置信，确定性变体负责"违规必现 + 放行恢复"的回归锚点——此前 #17/#18 两个 P0 场景只有概率性 stress 版（本文件 §10 门禁映射依赖其统计学置信，确定性变体补齐"必现"缺口）。

### 4.6 小状态空间线性化检查器（P3-y 新增，L3）

- 组件：`common/linearizability_checker.{h,cpp}`——记录每个 op 的调用/返回**时间戳区间** + 返回值 + affected id，回溯判定是否存在满足实时序约束（`≤H`/`≤RT`）的合法顺序；状态空间小（2 线程、容量 4、数十 op）时回溯可行；
- 挂载：`deque_test.cpp::LinearizabilitySmoke` 与 `mpmc_queue_test.cpp::LinearizabilitySmoke`（T1 层；穷举 + 有偏采样两档）；
- **适用范围声明**：L3 只覆盖小状态空间；大空间仍回 L1+L2，不得号称全规模线性化验证。

## 5. 活性测试与 watchdog 纪律

活性类场景（无挂死、有界完成）无法用普通断言表达"结束"，只能表达"**在时限内结束**"。纪律：

1. **watchdog 机制**：活性测试体内启动 deadline 守护（`std::future` + `wait_for(deadline)`）；主流程完成则取消；超时 ⟹ `FAIL()` + **现场 dump**（`stats()` 快照、各 worker 的 `wstats`、当前 pending/stopping/waiters 状态）——工程目标是"死掉的现场可定位到卡在协议哪一步"；
2. **超时即失败，不做重试判定**：任何"超时但勉强完成"的情形一律 FAIL；宁可落得严，活性回归的代价大于偶发 flaky；
3. ctest `--timeout` 为 watchdog 失效的最后兜底（§3）；
4. **活性测试清单**（实现时必须挂 watchdog）：`WakeupSuppressionStall`（场景 #13）、`DrainDuringShutdown`（#11）、`DrainNotifyRace`（#14）、`PoolSaturationSpill`（#15）、`SpillOverflowReentrancy`（#18，P3-s：drain 循环终止性 + 守恒；P3-ad：增补"容量 16 + BULK_LIMIT 32 连续 bulk"确定性变体）、`SpawnWhileIdleWakeup`（#19，P3-u：空闲池嵌套 spawn 的唤醒与并行度守卫）、`ShutdownGateRace`（#9）、`ShutdownRace`（#10）、`CrossPoolSubmit`（#20，P3-ad：跨池串计数的守恒 + drain 有界）、`RejectionRollbackNotifies`（#21，P3-ad：拒绝归零必须唤醒并发 drain，散落 fetch_sub 必现挂死）、`DrainDuringSubmit`（#7，P3-v：quiescence 语义——断言静止点返回、不断言快照屏障）、`WrapAround`（#5，追圈收敛的实测上限）、`SpuriousWake`（#12）、`FullScanCoversAllVictims`（P3-o 守护）。

watchdog 时限不可从基准数据推导（避免测试间依赖）；默认取"同机实测 20× median + 2s 保底"，实现时写入本节常量表。

## 6. 测试文件与清单落位

目录与主设计 §10.4 清单**一一对应**（清单即本节的结构来源，新增用例必须回写 §10.4 与 §10.3 场景表）：

```text
tests/
├── common/
│   ├── diff_oracle.h/.cpp        # §4 参照模型 + 守恒/顺序判定
│   ├── op_generator.h            # §4.3 op 序列生成器（seed 纪律）
│   ├── watchdog.h                # §5 deadline 守护
│   ├── test_hook.h               # §4.5 MAGPIE_TEST_HOOK 测试缝注册点（契约见 mpmc-queue-adr §6）
│   ├── linearizability_checker.h/.cpp  # §4.6 L3 检查器（时间戳区间回溯判定）
│   └── test_tasks.h              # 带 id 的 Task 测试件（唯一 id 分配器）
├── deque_test.cpp                # PushPopSequential / StealZeroContract(P3-ab 契约) / DifferentialRandomOps /
│                                 # TwoThreadPushSteal /
│                                 # LastElementRace(Stress) / BulkStealInterleave / SpillVsSteal /
│                                 # StealFromFullDequeStress(Stress，场景 #17，P3-r) /
│                                 # StealFromFullDequeDeterministic(场景 #17，P3-y：steal 缝确定性交错) /
│                                 # LinearizabilitySmoke(P3-y，L3)
├── mpmc_queue_test.cpp           # SingleThreadFifo / WrapAround / MultiProdMultiCons(+差分) /
│                                 # ProducerClaimStall(P3-x：MAGPIE_TEST_HOOK 缝的 reservation hole 确定性交错) /
│                                 # LinearizabilitySmoke(P3-y，L3)
├── event_count_test.cpp          # WakeupLossWindow / SpuriousWake；backend 参数化，见 §7
└── pool_test.cpp                 # 集成：SubmitAndDrain / FutureException / RejectionPolicies /
                                  # ShutdownGateRace / ShutdownRace / DiscardUnderLoad /
                                  # DrainDuringSubmit / DrainDuringShutdown / WorkerSubmitsChild /
                                  # WakeupSuppressionStall / DrainNotifyRace / PoolSaturationSpill /
                                  # SpillOverflowReentrancy(Stress，场景 #18，P3-s；P3-ad 增补容量16×bulk32 变体) /
                                  # SpillOverflowReentrancyDeterministic(场景 #18，P3-y：spill 缝确定性交错) /
                                  # SpawnWhileIdleWakeup(场景 #19，P3-u) /
                                  # BulkTransferPreservesFifoOrder(场景 #16，P3-w 更名，取代 GlobalFifoEndToEnd) / FullScanCoversAllVictims /
                                  # CrossPoolSubmit(场景 #20，P3-ad) / RejectionRollbackNotifies(场景 #21，P3-ad) /
                                  # BurstAcrossShutdown / DiscardRetryExhausted /
                                  # HandlerThrowsSwallowed / SoakLongRun(Soak，独立文件编译)
```

分层归属（GTest 后缀 + LABELS）在实现时按 §3 标注于各测试上，T2/T3 的候选 `LastElementRace`（百万次）与 `SoakLongRun` 已在上表标注。

## 7. 双实现同测（EventCount，D4 落地）

ADR-002 §6.3 的 D4 纪律执行方式：

- `event_count_test` 全部用例以 **backend 参数化**（`TEST_P` + `MAGPIE_EC_BACKEND` 环境变量或编译期混入）在 `futex` 与 `generic` 两实现上**跑同一套断言**；
- CI `ci-fast` job 必须两个 backend 都跑：futex 版在 Linux native 变体，generic 版作为 **TSan 证据来源**（TSan 不建模裸 futex syscall 的 happens-before，主设计 §10.4 平台局限——generic 版是协议正确性的可检测背书）；
- 任何 EventCount 协议改动：两 backend 同套测试全绿是合入门禁，缺一不放。

## 8. sanitizer 变体与平台证据

### 8.1 三变体映射（对应主设计 §16.1）

| 变体 | 构建选项 | 约束 |
|---|---|---|
| release | `-O2` | 基准对比与常规语义 |
| TSan | `ENABLE_TSAN` | ① DeathTest 禁用（`fork` 与 TSan 冲突，选型成本 §2）；② 压测迭代缩放：`MAGPIE_TEST_TSAN_SCALE`（默认 1/10，报告注明实际迭代数）；③ futex backend 的 happens-before 不可依赖 ⟹ generic backend 是 TSan 证据来源（§7） |
| ASan+UBSan | `ENABLE_ASAN`+`ENABLE_UBSAN` | soak 常跑变体；LSan 附着于 ASan（Linux），所有权泄漏由之兜底 |

### 8.2 ARM64 附加证据（放宽门禁用，一次性运行）

主设计 §10.4 平台局限条款的落地操作：

- 触发：仅当 W2.7 的某处内存序放宽动作准备落地时（ADR-001 §5.3 门禁），执行一次；
- 内容：ARM64 环境（物理机/云实例，clang TSan 或 GCC tsan aarch64）编出 TSan 变体，跑 `DifferentialRandomOps` + `LastElementRace` + `BulkStealInterleave`（TSan 压力迭代可缩放，但缩放系数需在报告中声明）；
- 归档：输出与工具链信息存档于 `results/arm64/`，作为该放宽动作 ADR 附录的附件；
- 资源不可得 ⟹ 该放宽动作**整体搁置**，不得以 x86 证据代替（与主设计 §10.4 同口径）。

## 9. 编写规范

1. 命名：`Suite_Scenario`（如 `ChaseLevDeque_PushPopSequential`），场景名与主设计 §10.4 清单名一致，禁止自造别名；
2. 隔离：每个 TEST 自建线程池与队列实例，**禁止跨测试共享可变全局状态**（顺序无关性保证）；共用只读常量放 `common/`；
3. 断言纪律：并发场景只断言守恒/不变量/活性（§4.2/§5），**不断言特定交错**（除严格单生产者顺序场景）；不做性能断言（§1）；
4. 失败报告：差分类失败附 seed 与 op log（§4.3）；活性失败附现场 dump（§5）；回归提交的 commit message 附复现命令（含 seed/环境变量）；
5. 新增测试的流程约束：先在主设计 §10.3 场景表登记"场景 ↔ 正确性机制 ↔ 测试"三列，再落代码——不允许出现无场景归因的测试；
6. 随机源一律经 `op_generator.h` 统一入口（拒绝测试内自造 rand），保证 seed 纪律单点生效。

## 10. 与阶段门禁的关系

- 映射规则：主设计 §16.4 每个工作项的"验收"列所点名的测试（集），其演出要求 = **对应层（§3）× 对应 sanitizer 变体（§8）全绿**；例：`W2.3` 的验收"deque_test 全绿（TSan），含批量窃取交错"= deque_test 的 T1 于 release+TSan 两变体绿 + `BulkStealInterleave` 在列；
- 门禁不闭合的后果：前置未过门禁不开展下一项（主设计 §16.4 规则），无例外通道；
- soak/ARM64 属"证据增强层"，不阻塞 W1~W2 日常推进，但 W2.7 的放宽动作缺 ARM64 证据不得落地（§8.2）。