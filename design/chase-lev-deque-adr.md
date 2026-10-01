# ADR-001：Chase-Lev 双端队列——批量窃取协议、内存序与溢出转移

- 状态：**修订中（v1.9，P3-ad 批次后待复核）**——核心 steal 锁存协议与 §4.4 论证成立；风险重心在 overflow 暂存与再发布生命周期（§4.6 已重写，待 SpillOverflowReentrancy 变体实测背书后恢复"已接受"）；内存序放宽部分为"已论证待验证"
- 日期：2026-09-21
- 修订：v1.1（2026-09-22）：P1-1 溢出不变量、P1-5 每槽一行定稿、P1-7 窃取量钳制、P2 测试名统一
- 修订：v1.2（2026-09-22）：P0-B pop 加了 seq_cst fence 基线（含屏障性质③与常规路径安全性重述）、§4.1 声称 1/2 重写为纯代数证明、新增跨线程配对屏障审计表、§6 同步 P2-E（整批 deadline）/P2-G（溢出后 notify）/P2-D（wstats_ 注入）；P3-j 删除 pop 的 b==0 早退
- 修订：v1.3（2026-09-29）：P3-q 批次——§5.2 无 fence 路线具体化（引用既有论文方向 + mfence 成本归因要求）、§8 布局对照实验升级为正式工作项（W2.9）
- 修订：v1.4（2026-09-29）：**P3-r 批次（P0 修复，场景 #17）**——steal 改为"候选先锁存、后 CAS 认领、失败整批丢弃"（§2/§3/§4.1 重构），新增 §4.4 环形槽复用与锁存安全性证明；§5.1 补锁存内存序注；§6 补 spill 免疫说明；§7 补 StealFromFullDequeStress
- 修订：v1.5（2026-09-29）：**P3-s 批次（P0 修复，场景 #18）**——deque 内禁止执行用户代码：spill 单次尝试入队、认领未转移项暂存 `overflow_` 移交池层 `drain_spill_overflow`（§6 改写）；§3 补"owner 操作协议原子性（不可重入）"显式假设；新增 §4.5 重入缺陷记录与修复论证；§7 补 SpillOverflowReentrancy
- 修订：v1.6（2026-10-01）：**P3-w 批次（承诺收窄）**——§7 测试更名 `BulkTransferPreservesFifoOrder`（原 GlobalFifoEndToEnd），口径收窄为"单批内 FIFO"（联动 mpmc-queue-adr v1.4 / 上位场景 #16）
- 修订：v1.7（2026-10-01）：**P3-y 批次（测试缝 + L3）**——§7 补 `StealFromFullDequeDeterministic` / `SpillOverflowReentrancyDeterministic`（steal、spill 缝的确定性变体，缝注册表见 test-plan §4.5）与 `LinearizabilitySmoke`（小状态空间线性化检查器）
- 修订：v1.8（2026-10-01）：**P3-ab 批次（契约修复）**——steal 的 `max_n == 0` ⟹ 立即返回 0 且零副作用（§2 决策表、§3/§4.1 前置）；`n==0 → 1` 归一化自此仅服务 `b−t==1`，两种"零"不得混同；§7 补 `StealZeroContract`
- 修订：v1.9（2026-10-01）：**P3-ad 批次（P0-1/P0-2 修复 + 缝位修正）**——§4.5/§4.6：溢出暂存改固定容量 noexcept 双缓冲、禁止 clear（容量上界三事实证明）；§7 `StealFromFullDequeDeterministic` 缝位改"锁存后、CAS 前"（旧缝位对应已废止读序）
- 上位约束：[magpie设计方案.md](magpie设计方案.md) §7.1、§10.1、§10.3 场景 #1–#4

---

## 1. 背景与问题

上位设计 §7.1.2 给出了 `ChaseLevDeque` 的实现骨架，本 ADR 落实三件事：

1. **批量窃取协议的正确性证明**：经典 Chase-Lev（Le et al. 2013）只证明"单元素窃取"；本项目扩展为"一次 CAS 抢占 `[t, t+n)` 批量窃取"，这是相对原算法的**唯一新增复杂度**（上位 §7.1.2 明确点名），必须给出完整的安全论证。
2. **内存序放宽论证**：初版 `top` 全 `seq_cst`；本文给出可放宽的目标形态与放宽的门禁条件。
3. **溢出转移细节**：`spill_lowest_half` 的语义细节、与窃取的并发关系、CallerRuns 兜底。

## 2. 决策摘要

| 项 | 决策 |
|---|---|
| 容量 | 固定容量（2 的幂），不做动态扩容（上位 §7.1.3） |
| 窃取粒度 | 批量：内部钳制 `n = min(caller_max_n, STEAL_CAP, (b-t)/2)`，`n ≥ 1`；一次 CAS 抢占 `[t, t+n)`。**P3-ab 契约**：`max_n == 0` ⟹ 立即返回 0、**零副作用**（不读槽、不 CAS、不动 top/bottom）；`n==0 → 1` 的归一化仅服务 `b−t==1` 情形，两种"零"不得混同 |
| steal 读序（P3-r） | **候选先锁存、后 CAS 认领、失败整批丢弃**——防环形槽复用竞态（§4.4）；替代 v1.0–v1.3 的"CAS 先、读后"（P0 缺陷） |
| pop 边界 | `b < t` 回正分支 + `b == t` 的 CAS 竞争分支 |
| 初始内存序 | `top` 全 `seq_cst`；`bottom` 本地写 `relaxed`、push 发布 `release`、窃取者读 `acquire` |
| 目标内存序（放宽后） | §5 表；放开前提见 §5.3 门禁 |

## 3. 协议与不变量

- 环形数组容量 `capacity = 2^k`（`mask = capacity - 1`）；`top` 与 `bottom` 为单调变化的 64 位逻辑索引，物理槽位为 `i & mask`。
- **I1（容量）**：`0 ≤ b − t ≤ capacity` 恒成立。
- **I2（槽内容）**：区间 `[t, b)` 内的槽持有未消费任务（按 `i & mask` 取模）；区间外的槽内容无意义。
- **声明即所有权（P3-r 修订：区分"锁存时刻"与"声明时刻"）**：所有消费动作必须先"声明"区间、后**使用**槽值；声明机制为
  - push / pop：owner 独占写 `bottom`（push 是 `+1` 发布，pop 是 `−1` 声明取走最右槽）；
  - steal / spill：CAS `top`（声明取走最左一段）。
  steal 是唯一**锁存先于声明**的例外（候选先读 → CAS → 失败丢弃，§4.4 证其安全）——"声明即所有权"约束的是**读取结果何时生效**（CAS 成功前锁存值不得被使用/发布），不是物理读取动作的先后。v1.0–v1.3 的表述"先声明、后读槽"对 steal 是错的：CAS 成功后 `[t, t+n)` 退出 `[top, b)`，owner 的 push 守卫随即放宽，可在窃取者读槽前覆盖之（环形复用，场景 #17 / P0 缺陷记录见 §4.4）。
- **pop 无 `b == 0` 早退**（P3-j）：骨架已删除该分支——`bottom == 0` 仅在池构造初期成立一次，不是常见形态；空队判定完全由 `b < t` 回正分支兜底（`bottom == 0` 时 `b = −1 < t`，直接回正）。保留该早退会误导读者以为 bottom 是有界计数。
- **owner 操作协议原子性（不可重入，P3-s 显式化）**：本文所有 owner 独占论证（§4.3"同一线程不会边 pop 边 spill"、§6 的免疫说明等）都依赖一个此前未言明的假设——**owner 的每个 deque 操作在其协议收口前不得执行任何用户代码**。用户代码一旦在半途执行并重入 push/spill，owner 的"单线程"将变为"单线程但可重入"，区间守恒与物理槽别名论证全部失效（v1.5 前的 spill 恰在此处违约，缺陷记录见 §4.5）。落地纪律：deque 内禁止 ①执行用户代码 ②任何可能回调用户代码的等待——唯一例外是 `gq_.enqueue`（无锁、无回调）。
- **线性化点**（与上位 §10.2 一致）：push 于 `bottom` 的 release store；pop/steal 于对 `top` 的成功 CAS（pop 的常规路径例外，见 §4.2）。

### 3.1 I1 的维持

| 动作 | 对 `b − t` 的影响 | 前提 |
|---|---|---|
| push | `+1`；写槽前守卫 `b − t ≥ capacity` 则先 spill | 守卫保证写槽时 `b − t < capacity`，写后 `≤ capacity` |
| pop | `−1`；或 `b < t` 时回正 `bottom = t`（变为 0） | 恒成立 |
| steal / spill | `top += n`，`b − t` 减小 | `n ≤ (b−t)/2` 且 `n ≥ 1`（spill 的 `n = (b−t)/2`） |

spill 成功移走 `(b−t)/2` 后剩余 `b − t = (b−t) − (b−t)/2 ≤ capacity/2`，故 push 溢出后重读一次 `bottom` 即保证可写。

**P1-1 真实不变量**：spill 返回 false 当且仅当并发窃取者已通过 CAS 推进 top——每次推进 ≥1（steal 的 n 被钳制 ≥1）——故 push 重读后必有 `b − top ≤ capacity − 1`，写槽安全（对应上位 §7.1.2"关键性质"）。注意此论证依赖 n ≥ 1 钳制，任何把窃取粒度改为可为零的优化必须先改写本不变量。

## 4. 批量窃取协议的安全证明

目标：**每个任务恰好被消费一次**（push 发布一次、恰好一个消费者成功声明并读走）。等价于证明：任意两个成功声明（pop 一个 / steal 一批 / spill 一批）的区间互不相交。

### 4.1 关键引理：区间声明互斥

pop 的动作序列（按程序序）：

```
P1. bottom_.store(b−1, relaxed)  // 声明 [b−1, b)
F.  std::atomic_thread_fence(seq_cst)   // P0-B 基线：把 P1 的减量钉在其后的 top 读之前（StoreLoad 序）
P2. t_p = top_.load(relaxed)      // 与 steal 的 CAS 竞争裁决依据（fence 已承担全部次序）
P3. 若 b−1 ≥ t_p：读槽 b−1；若 b−1 == t_p 走 CAS(top, t_p → t_p+1, seq_cst)
```

steal 的动作序列（P3-r 修订：锁存先于认领）：

```
S1. t_s = top_.load(seq_cst)
S2. b_s = bottom_.load(acquire)
S3. n = min(caller_max_n, STEAL_CAP, (b_s − t_s)/2)，不足 1 抬到 1   // 内部钳制（P1-7）
S3'. 候选锁存：out[i] = buf[(t_s+i) & mask].load(relaxed)，i ∈ [0, n)   // P3-r：先读后认领
S4. CAS(top, t_s → t_s + n, seq_cst)；失败 ⟹ 丢弃已锁存候选并返回 0   // 声明 [t_s, t_s+n)
```

**P3-ab 前置**：`max_n == 0` 时 steal 在 S1 之前即返回 0（§2 契约），不进入本节动作序列——声称 1/2/3 及 §4.3 综合结论只讨论 `n ≥ 1` 的认领情形，证明域不变；S3 的"不足 1 抬到 1"此后仅对应 `b_s − t_s == 1` 的来源。

声称 1/2/3 的证明只引用 S1–S4 的**声明语义**（t_s / b_s / n 与 CAS 结果），不含 S3' 的读取动作——S3' 位于 S2 与 S4 之间，不改变任何证明前提。故 P3-r 的读序重构不触及下述证明本体，仅新增 §4.4 覆盖此前缺失的**存储生命周期**论证（场景 #17）。

**证明约定（P0-B 修订）**：本节的论证**不做任何模型外的时序推断**（如"读到旧值 ⟹ 读先于写"不可推导），依赖三条事实：①`top` 只能经 seq_cst CAS 单调前进；②bottom 在本次 pop 期间只在 P1 处减 1（owner 独占），故窃取者的 S2 只能读到 `b`（减量前）或 `b−1`；③**屏障性质（Le et al. 2013 的 fence 语义，x86-TSO 下即 mfence 冲刷）**：pop 的 seq_cst fence 使其 bottom 减量的有效性先于 P2 的 top 读——若某窃取者的 S2 读到减量前的旧值 `b`，则该窃取者的 S1 只能发生在 pop 的 fence 生效（冲刷）之前，从而其 CAS 基点（top 值）不可能高于 pop 之后读到的 `t_p`。本条是**基线不可放宽项**：任何想移除 fence 的方案必须改用纯代数重证且过 §5.3 门禁。

**声称 1**：若 pop 走到 P3 的常规分支（`b−1 > t_p`），则任何成功 steal 的区间 `[t_s, t_s+n)` 满足 `t_s + n ≤ b−1`——由于区间**右开**，槽 `b−1` 永远不会被触碰。

证明（代数为主，仅 `t_s = b−1` 一处引用屏障性质③）：

- 情形 0：`t_s > b−1`。成功 steal 需要 `b_s > t_s ≥ b`，而 `b_s ≤ b`——矛盾，故此类 steal 不可能成功 ✓
- 情形 1：`t_s = b−1`。成功 steal 需要 `b_s > b−1` ⟹ `b_s = b`（S2 读到减量前的旧值）。由屏障性质③，该窃取者的 S1 发生在 pop 的 fence 生效之前，而 pop 的 P2 在 fence 生效之后——按真实时间 S1 先于 P2，则由 top 单调 ① 有 `t_p ≥ t_s = b−1`，与常规分支条件 `t_p ≤ b−2` 矛盾 ⟹ 该组合不可达 ✓（这正是 P0-B 修的洞：无 fence 时此情形可达成、造成槽 `b−1` 被 pop 与 steal 双认领）
- 情形 2：`t_s < b−1`。成功 steal 满足 `b_s > t_s`，`n ≤ (b_s − t_s)/2`：
  - 若 `b_s ≤ b−1`：`n ≤ (b−1 − t_s)/2`。当 `b−1 − t_s = 1`：`b_s ∈ (t_s, b−1]` ⟹ `b_s = t_s+1`、`n = 1` ⟹ `t_s + n = b−1`（右开不触）✓；当 `b−1 − t_s ≥ 2`：`t_s + n < b−1` ✓
  - 若 `b_s = b`：`n ≤ (b − t_s)/2` 且 `t_s ≤ b−2`，故 `t_s + n ≤ t_s + (b−t_s)/2 ≤ b−1`（等号仅当 `t_s = b−2` 且此时 `n = 1`，右开不触）✓

**声称 2**：若 pop 走 P3 的竞争分支（`b−1 == t_p`），任一成功 steal 要么其 CAS 与 pop 的 CAS 由 `top` 的原子性裁决（恰一方成功），要么其区间不触碰槽 `b−1`。

证明（穷举 steal CAS 的目标 `t_s + n` 与 pop CAS 目标 `b (= t_p+1)` 的关系）：

- `t_s + n == b`：双方对同一 CAS 竞争，恰一方成功 ✓
- `t_s + n > b` 且 `≠ b`：区间右端越过 b，即覆盖槽 `b−1` 的右邻也越过了 `b−1`；此时必有 `t_s ≥ b−1`（若 `t_s ≤ b−2`，则 `t_s+n ≤ t_s + (b_s−t_s)/2 ≤ t_s + (b−t_s)/2 ≤ b−1`，与前提矛盾——同声称 1 的代数）。`t_s = b−1`（即 `t_s == t_p`）时，steal 的 CAS 与 pop 的 CAS 都从 `b−1` 出发、目标不同，由 `top` 的 CAS 原子性恰一方成功 ✓；`t_s ≥ b` 时区间完全在 `b−1` 之右，不触碰 ✓
- `t_s + n < b`：即 `t_s + n ≤ b−1`，右开区间不触碰槽 `b−1` ✓

**声称 3**：`b < t` 回正分支（P3 前检查）保证 pop 不读窃取者已声明的槽——pop 读到 `t_p` 大于自己的 `b−1` 时，其声明的 `[b−1, b)` 已被覆盖在 `[t_old, t_p)` 的某次批量声明中，直接放弃并回正 `bottom = t_p`，恢复 I1 ✓。

**综合**：pop 与 steal 的声明区间要么不相交，要么共享同一 CAS 的裁决（恰一方成功）；steal 与 steal 之间由 CAS 天然互斥；spill 与 pop 之间由 owner 独占保证串行（同一线程不会边 pop 边 spill）。连同 §4.4 的存储生命周期论证（区间互斥保证"槽被谁消费"，锁存顺序保证"消费的是哪一代内容"——二者共同成立才推出下述结论）⇒ **无重复消费、无丢失**（push 之后每个槽恰好被消费一次）。

### 4.2 常规路径的安全性承载体（P0-B 修订）

pop 的常规分支（`b−1 > t_p`）没有 CAS，其安全性由 **P1 与 P2 之间的 seq_cst fence** 承担：fence 保证①bottom 减量先于 top 读生效（程序内 StoreLoad 序），②提供屏障性质③（上节约定）以排除"stale bottom + 常规分支"的组合（声称 1 情形 1）。P2 因此可以是最廉价的 `relaxed` load——它只需要拿到"此刻 top"的值，剩下的全部由声称 1 的代数完成。**该 fence 是初版基线（§5.1）的不可放宽项，不是 §5.2 的放宽目标**：本设计在 x86 上支付每次 pop 一次 mfence 的代价换取协议的成立（是否存在免 fence 的代数证明路线，见 §5.2 末尾的开放项）。

### 4.3 spill 的并发分析（场景 #4）

`spill_lowest_half` 与窃取者共享 `top` 的 CAS：spill 的 CAS（`t → mid`）与任意 steal 的 CAS（`t' → t'+n`）并发时，CAS 语义保证两者之一失败。失败方（spill）返回 false 让出（P1-1：此时窃取者已推进 top ≥1，push 重读 bottom 后必有空间，无需重试循环）——**无活锁**：每轮并发中至少一个 CAS 成功，`top` 单调前进，任何一方都不会永久饥饿。spill 成功后逐槽读取 `[t, mid)`（该区间已被本次 CAS 排除出所有消费者的视野），以 FIFO 顺序入全局队列（§6）。

### 4.4 环形槽复用与锁存安全性（P3-r，P0 缺陷记录与修复证明）

**缺陷记录（v1.0–v1.3，场景 #17）**：旧 steal 为"S4 CAS 认领 → 之后才读槽"。CAS 成功的瞬间，`[t_s, t_s+n)` 即退出 `[top, b)`，owner 的 push 守卫 `b − top ≥ capacity` 读到的是**推进后的 top**，于是可以在窃取者读槽之前合法写满并绕圈，覆盖这些物理槽。反例（capacity=4，t=0，b=4，全满）：thief CAS top 0→2 后被抢占；owner push 守卫 `4 − 2 < 4` 通过，写逻辑位置 4 = 物理 slot 0，覆盖 T0；thief 恢复后从 slot 0 读到的已是新任务 T4——**T0 永久丢失，T4 被两次消费**（thief 认领的 logical 0 与队列内的 logical 4 都指向它）。区间互斥（§4.1）在逻辑序号空间成立、在物理槽复用窗口不成立；I2 在"认领后、读取前"的窗口内被这个覆盖瞬时违反。

**锁存安全性证明（P3-r 修复的依据）**：窃取者的候选锁存 S3' 全部发生在 S4 的 CAS 之前。设锁存期间 top 恒为 `t_s`（CAS 未发生；其他窃取者若先推进 top，则本方 S4 必失败 → 丢弃，无需关心锁存值），证明 owner 在此窗口内**不可能**覆盖物理槽 `t_s + i`：

- push 覆盖物理槽 `t_s+i` ⇔ push 的写索引 bottom 到达 `t_s + i + capacity`（逻辑序号与物理槽同余）；
- owner 连续 push 第 k 次前，bottom = `b_init + k − 1`；第 k 次 push 的守卫检查基于当前 top——锁存窗口内 top 恒为 `t_s`，故守卫约束为 `(b_init + k − 1) − t_s < capacity`，即 `k ≤ capacity − (b_init − t_s)`；
- 而覆盖 `t_s+i` 需要 `k = t_s + i + capacity − b_init = capacity − (b_init − t_s) + i ≥ capacity − (b_init − t_s) + 0`——当 `i = 0` 时恰等于守卫允许的最大 k **加一**；
- 故覆盖所需的 push 次数恒比守卫允许的多 ≥ 1：**窗口内覆盖结构性不可能** ✓（这正是 push 守卫 `b − top ≥ capacity` 与"top 未推进"合力的效果；v1.0–v1.3 的错误在于把这道守卫的检查时刻放在了 top 推进**之后**）。

CAS 成功 ⟹ 锁存值与逻辑槽 `[t_s, t_s+n)` 的绑定关系在整个认领过程中未被破坏，锁存数组 `out[]`（本地内存）不受后续任何覆盖影响；CAS 失败 ⟹ top 已被他人推进，`[t_s, t_s+n)` 已整段退出 `[top,b)` 且此后任何写入都以新的 top 为守卫基准——锁存值要么已被合法消费、要么已过期，**丢弃即正确**。结论：成功的 steal 交付的恰是声明区间在 `t_s` 世代的内容；失败路径零副作用。 ✓

### 4.5 spill 重入执行用户代码的缺陷记录（P3-s，场景 #18）

**缺陷记录（v1.0–v1.4，P0）**：旧版 spill（§6 的 v1.4 前记述）在认领 `[t, mid)` 后对入全局失败的项执行有界自旋，自旋出界则 `run_one_local(x)` **就地执行用户代码**。此时 deque 协议悬在半空（认领区间已从 `[top,bottom)` 移除、剩余项未锁存），违反 §3 的不可重入假设：用户任务再 `submit` → TLS 快路径重入本 deque 的 `push`/`spill`。

破坏机制（capacity=1024 之例，锁存游标 vs bottom 推进的速率差）：

1. spill 入口处 `b − t = capacity`（push 守卫正是在此时触发）⟹ `mid = t + capacity/2`；
2. 内层 push 第 k 次写逻辑 `b + k − 1`，物理别名 = `t + k − 1`（因 `b ≡ t (mod capacity)`）；
3. 外层 spill 以游标 i 逐槽锁存。当某次逃逸任务累计提交数 r 超过已锁存数（如单个逃逸任务提交 ≥2 个子任务），存在 k 使物理槽 `t + k` 是**尚未锁存**的认领槽：内层 push 覆盖之 → 外层稍后锁存到**重入的新任务**，原任务永久丢失；且新任务同时活在新逻辑槽 `b + k − 1`（∈ `[mid, b+r)`）中 → 双重消费。与场景 #17 同源（物理别名 + 锁存时序），差别仅在"写者"是重入的 owner 自身；
4. 放大因素：P2-E 的整批共享 deadline 使首个逃逸项耗尽预算后，后续失败项近乎立即逃逸——连续多个用户任务在中途执行，暴露窗口被拉宽。

**修复论证（P3-s）**：把"执行用户代码"从 deque 协议内彻底移除。spill 对每项**仅单次尝试**入全局，失败项暂存 `overflow_`（deque 私有，仅 owner 触碰）；`push` 调用方与 worker 主循环 ⓪ 步以 `drain_spill_overflow` 收口：swap 接管批 → 池层有界自旋（P2-E shared deadline 上移）→ 仍失败者 CallerRuns 就地执行。正确性要点：

- **协议收口先于用户代码**：drain 运行时 spill 早已返回、deque 不变量已恢复；逃逸任务的任何重入 `submit → push → spill` 都是完整的、新鲜的下一次协议实例，各层均有状态提交点；
- **swap 交替防迭代器失效**：内层重入的新溢出追加到换入的 spare 缓冲，外层 drain 迭代本地批不受影响，下一轮 while 继续处理——配合"每轮至少消费一项"，任务集合有限时必然终止（用户代码无限自提交与 CallerRuns 嵌套同属 §9.5 既定取舍）；
- **计数守恒不变**：认领项在入池时已 `pending++`，drain 的执行路径经 `run_one` 配平（上位 §8.2）；`local_spills` 仍在 spill 入口记账、`rejected` 上移到池层逃逸口；
- **活性互保**：`overflow` 非空 ⇒ pending > 0，故 worker 退出判定与"入睡前必达的 ⓪ 步"共同保证溢出绝不滞留；滞留即意味着 drain/join 挂死，属实现回归，由 `SpillOverflowReentrancy` 守恒压测守护。
- **溢出暂存介质（P3-ad，P0-1/P0-2 修复）**：`overflow` 由 vector 改为**固定容量 noexcept 双缓冲**（`std::array<Task*, BATCH_CAP>` ×2 + 轮换指针 + 显式计数），理由与容量上界证明在 §4.6——spill 只 append、**禁止任何 clear**；swap 交替语义不变（take 时换缓冲，内层重入追加到 spare）。

### 4.6 溢出暂存的容量上界与 noexcept 论证（P3-ad，P0-1/P0-2 修复）

**P0-1（clear 丢任务）**：旧实现每次 spill 先 `overflow_.clear()`。当两次 spill 之间没有 drain 介入时（容量取下限 16、`BULK_LIMIT = 32`：bulk 路径连续 push 必触发两次 spill），第一批暂存在第二次 spill 的 clear 中被永久丢弃——认领任务泄漏。**修复**：spill 只 append、绝不 clear；drain 是唯一消费点。

**P0-2（线性化点后可抛）**：spill 在 top CAS 认领**之后**暂存。vector 扩容若于此刻抛 `bad_alloc`，已认领项既不在全局队列也不在暂存区、无法回滚——任务守恒破坏。**修复**：固定数组 + 显式计数，协议内全程 noexcept（`MAGPIE_ASSERT` 守护上界，config.h P3-ad）。

**上界证明（保守链，三项事实）**：

1. spill 每次认领 `(b−t)/2`；因 I1（`b−t ≤ capacity`），单次 spill 的 append ≤ capacity/2（且仅当 `b−t == capacity` 时取等）；
2. 两次 drain 之间的入队数有界：drain 位于 worker 主循环 ⓪ 步（5.3），快路径每 push 即自 drain（8.4）；故两次 drain 之间只有 ② 的 bulk 批（≤ `BULK_LIMIT`）或 ③ 的窃取批（≤ `STEAL_CAP`）在连续 push——期间 push 总数 ≤ `BATCH_CAP`；
3. **append 总量 ≤ 期间 push 总数**：每次 spill 总是先有入队把它触发（守卫 `b−top ≥ capacity` 首次越界），且单次 spill 认领数 ≤ `capacity/2`、此后每个再触发点之间至少要补 push `capacity/2` 个（spill 后保留最新半段）——按"认领数/触发间距"成对放缩，总 append ≤ 触发与填充所需的 push 总数。

由此 `overflow_count_ ≤ BATCH_CAP` 恒成立；`static_assert(缓冲容量 == BATCH_CAP)` + `MAGPIE_ASSERT(overflow_count_ < BATCH_CAP)` 双兜底，写满即证明失效（debug 断言；release 由证明覆盖）。测试：`SpillOverflowReentrancy` 增加"容量 16 + BULK_LIMIT 32 连续 bulk"确定性变体（配 fallback 使两次 spill 都产生暂存——旧实现必现丢失）。

## 5. 内存序设计

### 5.1 初版基线（正确的保守态；P0-B 修订）

| 操作 | 内存序 |
|---|---|
| top_ 的 CAS（steal/spill/pop 竞争分支） | `seq_cst` |
| pop 的 `top` 读 | `relaxed` + **前置 `seq_cst` fence**（P0-B 基线，不可放宽项，见 §4.2） |
| bottom_ 本地 store（pop 中途、回正） | `relaxed` |
| bottom_ push 发布 store | `release` |
| bottom_ 窃取者 load | `acquire` |
| buf_ 槽位读写 | `relaxed`（值的可见性由上述发布/同步关系承载） |

同步链（push→steal 可见性）：push 先写槽（relaxed），随后 `bottom store release` ⇒ 窃取者 `bottom load acquire` 与之同步 ⇒ 槽写入对窃取者可见。pop 读自己线程写的槽无需跨线程同步。**P3-r 锁存内存序注**：steal 的候选锁存（S3'，relaxed）沿用同一条可见性链——其正确性只依赖"S2 的 acquire 带来已发布槽内容"，与 CAS 无关；顺序约束仅要求锁存语句程序序先于 S4 的 CAS（seq_cst CAS 的屏障语义保证编译器不将前述 relaxed 原子重排跨越之；若未来按 §5.2 把 CAS 降级为 acq_rel，须重新论证此约束，禁止随降——降级动作须在本表登记）。

**跨线程配对屏障审计表（P0-A/P0-B/P3-t 的门禁交付物）**：全设计的三处"非 SC 写 → 后续读"跨线程配对——

| # | 配对 | 承担的可见性机制 | 若缺失的后果 |
|---|---|---|---|
| 1 | submit 入队的 release store → 等待者的探测读 | EventCount 的**无条件 epoch seq_cst RMW + 等待者 W5 SC 复查**的读-自同步（ADR-002 v2.3/P3-t）。v2.1 曾以"notify 开头的 seq_cst fence"承担本行，**已证伪**（fence 不跨对象建立同步；x86-TSO 是巧合）——本行登记的是发布机制而非屏障 | P3-t：任务对睡眠者永久不可见、全员入睡（若 epoch 改为条件式或后置于闸门，同一后果复活） |
| 2 | pop 的 `bottom` relaxed 减量 → pop 的 `top` 读 | pop 内 seq_cst fence（本文 §4.2） | P0-B：槽 `b−1` 被 pop 与 steal 双认领 |
| 3 | push 的槽写 → `bottom` release 发布 | release/acquire 配对本身（无需额外屏障） | 窃取者读到未发布槽内容 |

此表是 §5.3 门禁第一条（书面论证）的必填附件；任何新增跨线程配对都必须在此登记屏障来源。

### 5.2 目标形态（放宽，待验证后启用）

| 位置 | 初版（基线） | 目标 | 依赖的性质 |
|---|---|---|---|
| push 的 `top` load（溢出守卫） | `seq_cst` | `acquire` | 只需看到 CAS 的"最新 top"，无需全序 |
| push 的 `bottom` 发布 | `release` | `release`（不变） | 发布槽内容 |
| pop 的 `bottom` store + fence + `top` load | `relaxed` + seq_cst fence + `relaxed`（P0-B 基线） | **不动**——基线即终态 | 屏障性质③（Le et al. 2013） |
| pop 竞争分支 CAS | `seq_cst` | `acq_rel` | 与窃取者 CAS 竞争互斥即可 |
| steal 的 `top` load | `seq_cst` | `relaxed` + `acquire` fence | 先读值后读 bottom |
| steal 的 `bottom` load | `acquire` | `acquire`（不变） | 配对 push 的 release |
| steal 的 CAS | `seq_cst` | `acq_rel` | CAS 互斥 + 声明边界 |

**开放项（P3-q 修订：路线具体化 + 成本归因要求；P3-t 联动）**：pop 的每次 seq_cst fence 在 x86 上即一次 `mfence`，**落在每个任务的执行快路径上**（P3-t 起通知侧已无 fence——D0 废除，提交侧代价变为 epoch_ 行 SC RMW，与 pop fence 分开归因，见 benchmark-plan §8）——它是否吃掉 §14.1"单核百万 ops/s"预算的显著份额，必须以 `bench_empty_task` 的 perf 归因数据回答（benchmark-plan §8 观测项），而不是先验判断。改造路线确定为两条候选：
1. **免 fence 代数路线**：审查提出的纯代数 `b_s ≤ b_new + 1` 论证（声称不依赖 StoreLoad 屏障）；也须对照既有文献的无 fence Chase-Lev 变体（如 Correia et al. 2016 "Chase-Lev without fences" 系论证）交叉验证。产出物 = 本 ADR §4.1 情形 1 在无 fence 下不可达的完整证明 + §5.3 三门禁全过；
2. **更廉价屏障路线**：x86 上把 `seq_cst` fence 的代码生成为 lock 前缀哑指令（等价全屏障、SKL 上约 17 cycles vs mfence 约 33）——编译器扩展或内联汇编，同样须过 §5.3 门禁 3（基准）并登记跨线程配对屏障审计表（§5.1）。
无论哪条，在写出证明并通过门禁之前，pop 的 seq_cst fence 保留为基线；两候选合入 W2.7 的放宽验证工作流。

### 5.3 放宽门禁（三条并列必要条件，缺一不放；P0-5 修订：TSan 仅是必要不充分）

1. **书面内存模型论证**（必须——TSan 不验证 relaxed 推理的正确性）：每处放宽须先给出该 load/CAS 的 happens-before 逐条论证；
2. **TSan 干净**：§10.4 差分测试（`DifferentialRandomOps`、`LastElementRace` 百万次、`BulkStealInterleave`、`SpillVsSteal`）在**目标内存序构建**下 TSan 干净；
3. **基准无退化**：`bench_empty_task` / `bench_skew` 数据不劣于 seq_cst 版（吞吐与 p99 双看）。

每处放宽单独落一条 ADR 附录记录（改了哪个 load、依据 §5.2 的哪条性质、书面论证、TSan 与基准数据链接）。

## 6. 溢出转移细节（spill_lowest_half）

- **签名**：`bool spill_lowest_half()`——true=完成转移；false=CAS 竞争失败或无可溢出（P1-1）。
- **触发**：仅 push 的溢出守卫（`b − top ≥ capacity`）调用；owner 线程独占，与自身 pop 串行。
- **方向**：倒出**最老半段** `[top, mid)`，保留最新半段在本地——本地 LIFO 语义下"新任务数据更热"，倒老的才能既腾位又保命中（上位 §7.1.3）。
- **入全局队列的顺序**：按 `top → mid` 的 FIFO 原序逐个 `enqueue`，**每项仅单次尝试**（P3-s：deque 本体零等待、零自旋——自旋与逃逸全部移交池层，§4.5）；全局队列是有界 MPMC，判满即返回，容量语义由 seq 协议线性化。
- **认领未转移项与逃逸通道（P1-6/P2-E 的 P3-s 改写）**：入队失败项暂存 deque 私有 `overflow_`（仅 owner 触碰、swap 清理）；**deque 内禁止执行用户代码**（§3 不可重入假设，缺陷与论证见 §4.5）。池层 `drain_spill_overflow`（上位 5.3）在 spill 完全返回后收口：swap 接管批 → 以整批共享 deadline（`LOCAL_SPIN_US_TICKS`，P2-E"微秒级封顶"语义上移至每 drain 轮次）有界自旋重试入全局 → 出界者 CallerRuns 就地执行。**纪律保持**：本批已被 top 的 CAS 整体认领，逃逸必须把整批逐个执行/入队完毕，绝不允许中途放弃（否则认领未转移的任务永久丢失）——执行的场所变了（池层），义务不变；pending 已在 push 前计过数，由 `run_one` 递减配平（上位 §8.2）。这是全池饱和时的唯一出口，`PoolSaturationSpill` 断言封顶时长、`SpillOverflowReentrancy` 断言守恒与重入安全。
- **溢出后的唤醒（P2-G）**：本批 `transferred > 0` 时调用一次 `evc_.notify_one()`——与 §7.3.3"一切入队成功点"的口径一致，让空闲 worker 获得"全局队列刚被灌入半批"的信号（偏斜负载下避免损失并行度）。
- **计数（P2-D，P3-s 联动）**：`wstats_.local_spills`（构造函数注入的 per-worker 行）在 spill 成功入口处 +1，统计"溢出次数"而非任务数；`rejected` 上移到池层逃逸口（drain 内 CallerRuns 出界时）递增暴露异常——deque 不再记账 rejected。
- **与窃取并发**：见 §4.3；spill 返回 false 让出，无重试循环（由 push 的下一次守卫触发）。
- **P3-r 免疫说明（P3-s 修订前提）**：spill 的"CAS 先、读后"读序与旧 steal 同形，其安全性依赖**两个前提**：①认领者即 owner 线程 = 全池唯一写者（外部加塞只经 owner），读 `[t, mid)` 期间没有任何**跨线程** push——环形覆盖窗口不存在；②**deque 内不执行用户代码**（§3）——否则 owner 自身可重入成为写者，跨线程前提失效（场景 #18）。任何把"deque 写权限"扩展给非 owner 的设计（如未来 NUMA 分片）都必须先重估本条。
- **与全局队列的交互边界**：spill 是本地 deque 的唯一"出→全局"出口，与 submit 的"外部→全局"路径在全局队列的 seq 协议上线性化，无额外协调。

## 7. 测试对应

| 测试（上位 §10.4） | 覆盖性质 |
|---|---|
| `PushPopSequential` | LIFO 语义、容量边界 |
| `StealZeroContract` | P3-ab 契约：`max_n == 0` ⟹ 返回 0、零副作用（top/bottom 不变、无消费）；守护"两种零"不再混同 |
| `DifferentialRandomOps` | 随机 op 序列差分（多重集合比对） |
| `TwoThreadPushSteal` | 场景 #1：release/acquire 发布链 |
| `LastElementRace` | 场景 #2：声称 2 的 CAS 裁决（百万次） |
| `BulkStealInterleave` | 场景 #3：声称 1/3（批量区间与回正） |
| `SpillVsSteal` | 场景 #4：共享 CAS 无活锁 |
| `StealFromFullDequeStress` | 场景 #17（P3-r，P0 修复守护）：全满 deque 窃取 × owner 持续 push，守恒判定（每 id 恰消费一次）——§4.4 锁存安全性论证的实测背书 |
| `StealFromFullDequeDeterministic` | 场景 #17（P3-y 确定性变体，P3-ad 缝位修正）：steal 缝改在**候选锁存完成、CAS 认领之前**（旧"CAS 成功后、批锁存前"对应的是已被 P3-r 废止的读序，该时刻不存在）——门控暂停 thief，owner 触发 spill/push 推进 top，断言释放后 CAS 失败整批丢弃、守恒无损、原任务全在；§4.4 论证的"违规必现"回归锚点（缝注册见 test-plan §4.5） |
| `SpillOverflowReentrancy` | 场景 #18（P3-s，P0 修复守护）：全池饱和 + 逃逸任务重入提交 ≥2 子任务，守恒判定——§4.5 重入缺陷的实测背书 |
| `SpillOverflowReentrancyDeterministic` | 场景 #18（P3-y 确定性变体）：spill 缝（top CAS 认领成功后、逐槽转移前）门控暂停——断言 deque 收口先于任何用户代码、放行后恢复；§4.5 修复论证的回归锚点 |
| `LinearizabilitySmoke` | P3-y L3：小状态空间线性化检查器挂载（test-plan §4.6）；覆盖错误 empty 返回/线性化点时点类违规（差分 oracle 的失明区，见 test-plan §4.2.1） |
| `PoolSaturationSpill` | 场景 #15：全池饱和下 drain 有界自旋 + CallerRuns 逃生口不挂死（P3-s：含逃逸任务再提交变体，见上位 10.4） |
| `BulkTransferPreservesFifoOrder` | 场景 #16（P3-w 更名）：单 worker 受控下本地 LIFO 执行序经 bulk 逆序入队保持**该批内** FIFO（上位 5.3，P1-3）；全池执行序不承诺 |

## 8. 后果与开放性

- **受益**：单元素→批量窃取把"每任务一次 CAS 竞争"摊薄为"每 n 个任务一次"，是偏斜负载下可控尾延迟的关键（对应上位 §14.1）。
- **代价**：pop 增加 `b < t` 回正分支；协议证明复杂度上升（本文 §4 即为代价的书面偿付）。
- **开放项**：
  1. 深 deque 场景若被 benchmark 证伪，回到"动态扩容 + top 标记位防 ABA"（上位 §7.1.3 演进空间）；
  2. 布局对照实验（P3-q 修订：升级为正式工作项 W2.9）——"每槽一行"（P1-5 定稿；64KB/worker 超出 L1d 常驻）与"紧凑布局（每槽 8B）"为并列对照变体，由 bench_fine_grain/bench_empty_task 缓存 miss 数据裁决；全局队列布局同批裁决（mpmc-queue-adr §7 联动）；
  3. §5.2 放宽落地后，回填本条以"已放宽"状态并附数据。