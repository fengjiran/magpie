# ADR-002：EventCount——每 worker 等待状态字与关闭握手

- 状态：**v3.2，2026-11-05：M5 原有 Linux/generic 门禁已完成；本轮增加关停停车、完整退出复查及 pending/gate 最终通知，验证见 [M6 修复记录](../docs/milestones/M6-review-fixes.md)。** 历史原生证据见 [M5](../docs/milestones/M5.md)。
- 上位约束：[magpie设计方案.md](magpie设计方案.md) §5.3、§7.3、§9。
- 依据：[审核报告](review-2026-10-03.md) R5/R6。撤回 v2.x 共享睡眠字证明；generic 的额外 mutex 不能替 Linux 版作证。

## 1. 问题

旧共享 futex_word 可被另一个等待者写回旧 key，使通知已结束后仍能匹配入睡。全部 seq_cst 也不能消除这一状态机错误。

仅在 prepare_wait 前检查 stopping，遗漏“检查 false → shutdown 最终通知 → 才准备等待”窗口。

## 2. 结构与边界

保留一个池级 EventCount：64 位 epoch、SC 注册数、构造时稳定分配的 N 个 WaitSlot。每槽服务唯一 worker 的一次未结束注册。

word 是 uint32_t 状态 Idle=0、Waiting=1、Signaled=2，**不是 epoch 低 32 位**。每槽独立 futex 地址；通知侧 CAS Waiting→Signaled。等待侧仅在注册开始写 Waiting、结束写 Idle；不回写共享 generation，不存在低 32 位 generation 回绕。

通知入口无条件执行 epoch SC RMW。注册数闸门仅省扫描；有注册者时从 round-robin 起点扫描 N 个槽。notify_one 标记一个槽即停止；notify_all 不走闸门，逐槽标记。O(N) 扫描是实际代价，不能宣称无热点。

所有 WaitSlot 在启动 worker 前构造，运行中不移动，join 后才销毁。这里独立的是停车地址，不是 N 个独立 epoch 或提交路由。

## 3. 顺序约束

主设计 §7.3 为唯一代码骨架：

```text
W1 key = epoch.load(SC)
W2 waiters.fetch_add(1,SC)，本 worker word.store(Waiting,SC)
W3 stopping.load(SC) && gate.load(SC)==0 && pending.load(acquire)==0 → leave 并退出
W4 本地/全局/全 victim 最后探测；只认领，不执行用户代码
   命中 → leave；返回主循环后再执行
W5 epoch.load(SC) != key → leave 并返回重扫
W6 backend_wait(slot,Waiting)
W7 word.exchange(Idle,SC)，waiters.fetch_sub(1,SC)，检查配平

N1 epoch.fetch_add(1,SC)（上限前 fail-fast；无条件、先于闸门）
N2 notify_one 读 waiters(SC)，0 则返回
N3 扫描 slot，CAS Waiting→Signaled(SC)
N4 对成功标记槽发 wake；one 结束，all 继续

C1 shutdown 持 drain mutex 发布 stopping(SC)，解锁后立即 notify_all
C2 pending.fetch_sub(acq_rel) 的 1→0，持现有 drain mutex 检查 stopping(SC)，解锁后按结果 notify_all
C3 gate.fetch_sub(SC) 的 1→0，若 stopping(SC) 则 gate.notify_all（控制线程）和 EventCount.notify_all（worker）
```

每 worker 不嵌套注册；命中、完整退出、epoch 变化、EAGAIN/EINTR、spurious 均配平注册数。停止后仍有 pending/gate 时可等待。通知只要求重扫，不代表任务所有权。入队发布先于通知；不能先执行可能阻塞的用户代码再补通知。C2 在 drain mutex 解锁后执行，注册期间仍不得通知或运行用户代码。

## 4. 正确性依据与证明义务

### 4.1 旧字覆盖窗口

本 worker 的 Waiting store 在 W5 之前。若 W5 读旧 epoch，通知 N1 位于其后，Waiting 已发布；通知可标记 Signaled。其他 worker 只改自己的槽，不能把本槽恢复为 Waiting；本 worker 在 W6 前不会重新注册。

标记先于 wake：已入内核则被唤醒；尚未进入则 futex 的 Waiting 比较失败。这依赖同一注册内的单向状态推进，不依赖内核睡者数等于用户注册数。

notify_one 只选一个注册者，不是 broadcast。未选中的注册者若 W5 在 N1 后会见 epoch 变化；已通过 W5 者可以继续睡，由被选中者承担后续进度。池级活性依赖线程继续调度、用户任务最终返回、过门提交完成发布，不承诺非协作用户代码的有界尾延迟。

### 4.2 注册闸门

N2 读 0 时，无尚未注销的注册计数在它之前。计数++/Waiting 发布间暂停的 worker，其 W5 将看到通知新 epoch；扫描遇 Idle 因此不丢醒。

注销先 Idle、后计数--。CAS 与注销竞争允许多一次无效 wake；同一注册内禁止重新写 Waiting。旧通知延迟到下一次注册时，只能额外 signal/wake，不能撤销新通知。

### 4.3 关闭

worker 按 stopping SC → gate SC 为零 → pending acquire 为零裁决退出。若仍有执行任务或在途提交，停止后的 worker 可重新停车，最后一次归零必须唤醒它。

最终状态/通知在 W3 前：注册后的完整退出检查拦截，即使 key 已包含最终 epoch。最终状态在 W3 后：W5 若读取到最终通知或其后继 epoch 则不阻塞；否则 Waiting 已发布，由对应 notify_all 的 CAS 或注销覆盖。尚未入内核则 Signaled 使比较失败。

pending 与 gate 必须分别通知。拒绝提交的 capture 析构可以持 gate 而从未增加 pending；CallerRuns 也可能先 pending 归零再离门。只补其中一种通知不足以保证 join。

C1/C2 还必须用同一个 drain mutex 配对。若最终 pending 写零后读到 stopping=false，随后 C1 取得该 mutex，写零经 mutex happens-before 进入 C1 的 epoch 发布，晚注册者才能排除旧 pending；若 C1 先取得锁，则 C2 必见 stopping=true 并补通知。单靠 SC stopping 的总序无法把另一个 acq_rel pending 的写入传递给晚注册者。首次 closing 模型实际找到了这个反例，不能用 x86 原生测试通过代替该证明。gate 的修改与退出读取均为 SC，C3 的停止检查不额外取 drain mutex。

构造失败仍 store stopping → notify_all → join，此时 gate/pending 为零。注册后的完整退出检查仍是必要步骤，不能只依赖注册前提示。连续拒绝流、永久阻塞用户任务或线程不被调度不承诺有界退出。

同步 shutdown 对非零 gate 调用 C++20 atomic wait，醒后重新 load(SC) 裁决零；标准 wait 的值检查覆盖通知先于阻塞。控制等待者与 worker 使用不同停车对象，C3 两种通知不可互相替代。gate_wait.c 抽象标准库的原子比较/停车边界，覆盖两个控制等待者，不宣称验证具体标准库实现或连续拒绝流的 ABA 活性。

### 4.4 可见性与有限状态

队列发布先于 epoch 的 release RMW；W5 acquire 读到该 RMW 或后继 RMW 时取得发布可见性。醒后仍由队列自身 acquire/CAS 取得所有权。

uint64_t epoch 上限前 fail-fast，不允许回绕。word 仅有注册生命周期状态，不承载有限位 generation。

以上是协议的书面依据；下述有界模型补充注册/扫描/注销/重注册、并发通知和关闭证据，不标记为完整形式证明。

当前 C11 投影已覆盖 N=1/2/3、两轮注册、并发通知、抽象 compare/park/wake，以及 R5/R6 正反例。它不含真实 syscall 或整个调度器。M5 审核发现整批再发布的任务迁移循环，池层现改为保留一个执行项，只发布剩余项；线程继续调度、用户任务返回和发布完成仍是活性前提，原语唤醒正确不能代替任务推进。

## 5. 后端

Linux：FUTEX_WAIT_PRIVATE 比较 Waiting，FUTEX_WAKE_PRIVATE 唤醒已标记单槽。EAGAIN/EINTR 返回并注销；其他 errno 记录后 fail-fast，不以空转掩盖错误。构建检查 atomic<uint32_t> 的 lock-free、4 字节尺寸/对齐和平台 ABI 可供内核访问，不把 Linux ABI 当作所有 C++ 平台保证。

generic：每槽独立 mutex+condition_variable。等待持该 mutex 调用 cv.wait(lk, word!=Waiting)；通知在同一 mutex 下做 CAS/notify。epoch、注册数与状态规则不变，mutex 仅实现该后端的等待握手。

参考 [Linux futex 手册](https://man7.org/linux/man-pages/man2/futex.2.html)。generic 是补充测试，Linux syscall 窗口必须在原生 Linux 独立验证；TSan 不证明 futex 活性。

## 6. 测试与性能

必测：SeparateWaitWords、WakeBeforeKernelWait、ShutdownBeforeRegistration、ShutdownAfterRegisteredCheck、ConcurrentNotifyRegistration、RegistrationBalanced、EpochLimit、SpuriousWake、GenericPredicateHandshake、SpawnWhileIdleWakeup。

v3.2 补 FinalPendingZeroWakesClosingWorker、FinalPendingZeroBeforeKernelWait、FinalGateZeroWakesClosingWorkerWithoutPending；测试在额外 shutdown/析构前等待全部 worker 退出，防止收尾通知掩盖缺陷。closing.c 投影覆盖 pending/gate 最终转换与晚注册，分别删除 pending 通知、gate 通知、退出重查以及 C1/C2 的 mutex 配对作指定反例；范围与结果见 M6 修复记录。

缝：前置 stop 检查后、注册计数++后、W3 后、W5 后/W6 前、通知 CAS 后/wake 前。强制旧方案丢醒调度，不以随机压测替代。

wakes 记 backend wake 调用；wake_threads 记 Linux syscall 返回和，generic 为 optional/不可用。post_wake_* 记 backend 返回后首轮探测，包含 EAGAIN/EINTR/spurious，不能称内核有效唤醒率；pre_sleep_scan_* 单独记录。

必采 epoch、注册数、扫描长度、每槽状态行和 syscall 数。无注册者仍有 epoch 写；bitmap、条件式 epoch、通知抑制均为未来独立优化，须重证以上协议。
