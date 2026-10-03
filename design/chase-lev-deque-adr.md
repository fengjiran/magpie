# ADR-001：有界 Chase-Lev deque——单元素认领与池层溢出

- 状态：**已修订，待实现验证（v2.0，2026-10-03）**。
- 上位约束：[magpie设计方案.md](magpie设计方案.md) §5、§7.1、§10。
- 依据：[审核报告](review-2026-10-03.md) R1–R4。撤回 v1.x 批量 CAS 区间互斥及 overflow 容量证明，旧证明不能作为实现依据。

## 1. 问题与约束

固定容量、一个 owner 做 try_push/pop、多名 thief 做 steal。每个成功操作只移交一个 Task*，每项恰有一个最终执行者或丢弃者。deque 不执行用户代码，不依赖全局队列、唤醒器、统计或拒绝策略。

旧半批反例：thief 在 top=0/bottom=8 时锁存四项，owner 在其 CAS 前连续 pop 五项，top 保持 0；thief CAS 0→4 仍成功，重复交付 T3。一个 steal 可跨越多次 pop，增加屏障或 CAS 前再读 bottom 不能修复。

## 2. 决策

| 项 | 决策 |
|---|---|
| push | bool try_push(Task*) noexcept，满返回 false、零所有权变化 |
| pop | owner 单元素 LIFO；唯一元素与 thief CAS 竞争 |
| steal | Task* steal() noexcept，单元素 FIFO；nullptr 表示空或本次竞争失败 |
| 容量 | 校验并向上取整为 2 的幂，池层最低 16；不动态扩容 |
| 溢出 | 池层把**当前待提交任务**转投全局，失败则 CallerRuns；不迁移旧半批 |
| 统计 | 池层在本地 try_push 失败时记 local_spills；deque 不记统计 |

池层最多重复 STEAL_CAP 次单元素 steal，每项都有独立 CAS；首个失败即停止。全局 dequeue_bulk 的 limit 钳制为 min(BULK_LIMIT, local_capacity)。仅本地为空时接收，整批转移完成前不执行用户代码。

删除 spill_lowest_half、overflow 双缓冲、take_overflow 和 drain_spill_overflow。R3/R4 通过消除半批暂存协议修复。内部局部满是已接受任务的调度降级，不走配置的 Abort/DiscardOldest，始终以完成任务为义务。

## 3. 协议与不变量

代码唯一来源为主设计 §7.1.2。

- 无 owner 操作进行中的稳定边界：0≤bottom−top≤capacity。两个原子的任意并发快照不构成有效断言点。
- pop 减 bottom 后、裁决/恢复前允许 bottom<top，这是协议中间态。
- try_push 满时不写槽，不发布 bottom，不转移传入任务所有权。
- 普通 pop 的 bottom 减量由双侧 SC fence 协议保护；最后项 pop 和 steal 由 top CAS 裁决。
- pop CAS 失败会改写 expected t；bottom 恢复到稳定本地索引 b+1，不能使用 t+1。
- thief 在 CAS 前锁存原子槽指针，失败后丢弃；成功后才能解引用。失败者绝不使用可能已被其他消费者释放的候选。
- 槽必须是 atomic<Task*>：失败 thief 可能仍读取被 owner 重用的物理槽。
- deque 操作内禁止用户代码；池层 CallerRuns 发生在 try_push 已返回之后。

单元素 steal 跨多次普通 pop 时，目标仍只是旧 top 的一个槽；owner 要取得该槽必须到达最后项 CAS 分支。因此不再发生旧半批覆盖多个普通 pop 的问题。弱内存正确性仍须核对下节完整屏障。

## 4. 内存序基线与资料

| 操作 | 基线 |
|---|---|
| push top load | acquire |
| 槽写 | relaxed，后接 release fence、bottom release store |
| pop bottom 减量 | relaxed → SC fence → top relaxed load |
| pop 最后项 CAS | 成功/失败均 seq_cst |
| steal 取边界 | top acquire load → SC fence → bottom acquire load |
| steal CAS | 成功/失败均 seq_cst |
| 候选槽读 | relaxed；发布可见性由以上协议承担 |

保留 pop 与 steal **两侧** SC fence；不能用单侧屏障代替。发布、槽复用、失败候选规则均是证明义务。

一手对照：[Taskflow BoundedWSQ](https://taskflow.github.io/taskflow/wsq_8hpp_source.html)。只采用结构，不引入第三方依赖；正式实现记录对照源码 commit。上游源码存在不等于本实现已验证。

放宽门禁：标准内存模型书面论证、有界模型检查、对应 sanitizer 测试、ARM64 实测、同日 benchmark。TSan 不证明原子算法正确；不预先认定 acq_rel CAS 或免 fence 变体等价。

## 5. 索引与布局

top/bottom 用 int64_t；产生超出范围索引前必须执行 release 也有效的 fail-fast 检查，禁止 signed overflow。不在运行中重置索引，避免 thief 快照 ABA。上限测试用受控初始化点，无需运行 2^63 次。

默认紧凑原子指针槽，top/bottom 分行；每槽一行作为 W2.9 对照。缓存行大小采用受支持的构建配置及 feature-test fallback，不假定编译器一定提供 hardware_destructive_interference_size。

## 6. 验证与接受条件

| 测试 | 要求 |
|---|---|
| PushPopSequential / TwoThreadPushSteal | LIFO / FIFO 单项语义与发布 |
| StealAcrossOwnerPops | thief 锁存后暂停，owner 完成五次 pop，再放行；不重复消费 |
| LastElementCasFailureRestoresBottom | thief 赢最后项 CAS，再放行 owner；恢复空，无幽灵项 |
| StealFromFullDequeDeterministic | 槽复用下失败 CAS 不使用过期候选 |
| LocalOverflowDefaultCapacity | 满 1024 deque + 满全局，新任务 CallerRuns，旧任务仍在本地 |
| BulkReceiveCapacityBound | 容量 16、BULK_LIMIT 32，接收量≤16 |
| CallerRunsReentrancy | deque 完整返回后才执行嵌套提交 |
| DifferentialRandomOps / LinearizabilitySmoke | 守恒及包含竞争失败语义的历史检查 |
| DequeIndexLimit | 上限前确定失败，无 signed overflow |

旧 BulkStealInterleave/SpillVsSteal/SpillOverflowReentrancy 由上述测试替代；旧缺陷保留在审核报告。实现、模型检查与平台证据齐全前不标记“已验证”。
