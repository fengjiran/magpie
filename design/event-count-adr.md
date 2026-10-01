# ADR-002：EventCount 唤醒器——epoch 协议证明、waker 部署裁决与 futex 移植层

- 状态：**已接受（v2.5，P0-1/2/3/4、P0-A、P3-p/P3-q/P3-t/P3-u/P3-ad 修订后；核心协议成立）**；初版共享 waker；每 worker 方案为已论证的演进备选
- 日期：2026-09-22（v2 修订：等待侧顺序、waiters_ 内存序、notify 闸门、注册先行纪律；v2.1 修订：notify 前置 fence（P0-A）、闸门与垂直可见性论证；v2.2 修订：P3-p 标准口径联动——构建标准定为 C++20，正文"模型"表述不再锚定具体标准版本号；P3-q 修订：§3.5 截断边界表述由"论证"收口为"工程论证"，并补 notify 无上限来源时的重估条款）
- 修订：v2.3（2026-09-30）：**P3-t 批次（P0 论证缺陷修复）**——v2.1 的 D0 fence 方案证伪：seq_cst fence 无法跨原子对象建立可见性/同步（标准模型下"入队结果在闸门判定前对全体可见"不可推导，x86-TSO 是硬件巧合），notify 改为**无条件 seq_cst epoch RMW 先行、闸门降级为只裁决 wake**（R 先于 G 为硬条款）；§2 决策摘要、§3.1 序列、§3.2 D0/D2'、§3.3 引理 1/2 与主定理、§4 N0 段按 SC 二择重写；联动 ADR-001 屏障审计表 #1 与 benchmark-plan §8
- 修订：v2.4（2026-09-30）：**P3-u 批次（场景 #19，P0 缺陷收口）**——D2' 覆盖 worker 内快路径：废除主文档 §8.4 旧版"无需 notify"例外（sleeping 窃取者不因本地入队被唤醒，嵌套并行退化为单 worker 消化；父任务阻塞时构成结构性死锁）；§4 增补快路径统一口径与"轻量变体必须重证"的立项前提
- 修订：v2.5（2026-10-01）：**P3-ad 批次（P0-3 修复 + 两处清理）**——§4 增补 drain 再入队"批发布 + 一次 notify"口径（再入队点也属 D2' 一切入队点管辖；旧实现无唤醒，全睡时溢出项长滞）；§5.2 切换判据改 post_wake 口径（旧 wakes_effective 测的是入睡前扫描，语义错位）；§8 废止"无等待者只读比较"陈旧文字（v2.1 口径，防经"省一行写"优化把场景 #13 引回）
- 上位约束：[magpie设计方案.md](magpie设计方案.md) §7.3、§5.3（block_on_eventcount 纪律）、§10.3 场景 #8/#11/#12
- 修订记录：v1 存在三处真实缺陷（等待侧"复查先于写睡眠字"的丢醒窗口、`futex_word_` 非原子的数据竞争、`waiters_` relaxed 导致 notify 读到陈旧 0）与一处策略矛盾（深度阈值抑制 notify 破坏活性），v2 全部修正；v1 的"引理 2 情况 2"证明（把用户态 waiters_ 计数当作内核等待队列）作废。v2.1 修正 v2 自身引入的缺陷（P0-A）：闸门前置移除了提交侧唯一 StoreLoad 屏障，且旧引理 1 把 release store 视作参与 SC 全序——两者同源，一并修正为 D0 fence + 引理 1 新表述。**v2.3 推翻 v2.1 的 D0 fence 方案（P3-t）**：该版引理 1 断言"fence 使入队结果在闸门判定前对全体可见"，属跨对象的硬件直觉（x86 mfence 效果），C++ 内存模型无此规则——fence 的 synchronizes-with 只经同一对象上的读-写对建立；v1.0 的"无条件 epoch++"结构上本是正解（丢失的是正确证明而非结构），本版以无条件 seq_cst RMW + SC 二择复证收回该结构。

---

## 1. 背景与问题

上位设计决定用 EventCount + futex 替代 mutex + condvar。本 ADR 落实三件事：

1. epoch 协议的**不丢唤醒**证明（含前提条件的精确表述与 v2 顺序条款）；
2. **共享 waker vs 每 worker waker** 的裁决与演进判据；
3. futex 移植层的语义与实现约束。

## 2. 决策摘要（v2）

| 项 | 决策 |
|---|---|
| 部署 | 初版全池**共享一个**；`notify_one` 一次放行一个 |
| 等待侧顺序 | ① 拍 epoch → ② 注册（waiters_++）→ ③ **全量探测** → ④ 写睡眠字 → ⑤ 复查 epoch → ⑥ futex 睡 → ⑦ 注销 |
| 内存序 | `epoch_` / `waiters_` / `futex_word_` **全部 seq_cst**（等待者侧仅睡眠/唤醒路径；epoch 的 RMW 自 P3-t 落在提交路径，成本口径见上位 §7.3.3） |
| notify 策略 | 提交侧**无条件调用** `notify_one`；内部**无条件** `epoch_.fetch_add(seq_cst)` **先于** `waiters_==0` 闸门（R 先 G 是硬条款）；闸门只裁决是否 futex_wake（无深度阈值、无抑制；v2.1 的 D0 fence 已废除，P3-t） |
| 核心纪律 | **注册必须先于最后一次探测**（活性论证根基，D1'）；**epoch 推进无条件且先于闸门**（P3-t，取代 D0） |
| 移植层 | Linux 直用 futex；generic 版 mutex+condvar 语义等价，**双实现同测为门禁**（TSan 不覆盖裸 futex） |

## 3. 协议、纪律与证明

### 3.1 状态与调用序列

```cpp
class EventCount {
    std::atomic<std::uint64_t> epoch_;     // 世代；仅 notify 递增；seq_cst
    std::atomic<std::uint32_t> waiters_;   // 注册等待者计数；seq_cst（P0-3：必须 SC）
    std::atomic<std::uint32_t> futex_word_;// 睡眠字；seq_cst（P0-2：必须原子，杜绝数据竞争）
};
```

worker 侧（`block_on_eventcount`，与上位 §5.3 一一对应）：

```
W1. key = prepare_wait()            // = epoch_.load(seq_cst)
W2. enter()                         // waiters_.fetch_add(1, seq_cst)
W3. 全量探测（本地/全局/全 victim 随机扫描）
    命中 ⟹ leave()（fetch_sub）并返回
W4. futex_word_.store(key 低 32 位, seq_cst)
W5. if (epoch_.load(seq_cst) != key) { waiters_--; return; }   // 复查
W6. futex_wait(&futex_word_, 低32(key))
W7. waiters_.fetch_sub(1, seq_cst)
```

notify 侧（P3-t 修订：N0 废除、epoch RMW 无条件先行）：

```
N0. ——（废除，P3-t：seq_cst fence 不跨对象建立可见性，见 §3.3 旧引理 1 的证伪记录）
N1. e = epoch_.fetch_add(1, seq_cst) + 1              // 无条件；"入队发布 → 睡眠者"的可见性锚点（R）
N2. if (waiters_.load(seq_cst) == 0) return           // 闸门降级：只裁决是否 futex_wake（G）
N3. futex_word_.store(低32(e), seq_cst)               // 改字：令未入队者的值比对失败
N4. futex_wake(&futex_word_, 1)
```

### 3.2 依赖的调用纪律（正确性前提，实现评审 checklist）

- **D0（已废除，P3-t）**：v2.1 的"notify 首操作为 seq_cst fence"与其论证（fence 使入队结果先于闸门判定对全体可见）**不成立于 C++ 内存模型**——fence 的 synchronizes-with 只经同一原子对象上的读-写对建立，"跨对象全体可见"无此规则（x86-TSO 上 mfence 碰巧给出该效果，ARM64/PPC 可达丢醒交错）。其职责由 D0' 的无条件 epoch RMW 取代；实现不得因"性能理由"重新引入 fence 并宣称其有正确性含义，也不得在缺失 D0' 时以 fence 替代。
- **D0'（epoch 推进无条件且先于闸门，P3-t）**：`notify_one()` 内 `epoch_.fetch_add(1, seq_cst)` 必须无条件执行，且程序序**先于** waiters_ 闸门读（R 先 G）。理由见 §3.3 引理 2：若 R 后置或改为条件式（"有等待者才推进"），闸门读 0 与"注册在途"的窗口将使等待者 W5 读到旧 epoch 后入睡且无人唤醒——v2.1 丢醒窗口从本路径复活。此条是 D1' 之外的第二个活性根基。
- **D1'（注册先行）**：W2 必须先于最后一次任务探测（W3）。若实现把探测放回注册之前，"最后探测已过、待注册"的窗口使 N2 闸门跳过唤醒成为可能——丢醒。这是 v2 对 v1 协议最重要的结构改动。
- **D2'（一切入队点无条件 notify）**：一切"入队成功"之后必须调用 `notify_one()`——**含 worker 内快路径（P3-u）**：主文档 §8.4 旧版"无需 notify：本人正在执行任务"的例外已废除（该论证只对非阻塞嵌套成立；sleeping 窃取者不会因本地 deque 入队被唤醒，嵌套并行退化为单 worker 消化，父任务阻塞时更构成场景 #19 结构性死锁）。任何入队点省略 notify 都会让睡眠中的 worker 失去唤醒信号，且**禁止绕开 notify_one 直接判定 waiters_ 或直接动 epoch_/futex_word_**。快路径通知成本已被 P3-t 统一为"epoch_ 行提交率写入"；轻量变体（仅空→非空通知 / 仅 waiters_>0 才 wake）属 W2.10 立项项，落地前必须重证场景 #19 仍闭合。
- **D3（关停期不睡）**：`stopping` 置位后 `block_on_eventcount` 直接返回（上位 §5.3），避免"drain 期无人再 notify"的挂死（场景 #11）。
- **D4（双实现同测）**：futex 版与 generic 版必须通过同一套测试（§7）。

### 3.3 引理与主定理（v2.3，P3-t 重写）

**引理 1（epoch 锚定，取代 v2.1 的"D0 可见性"引理）**：设提交 S 的入队发布为 E（队列槽位上的 release store，或与其 release 序列等价），S 的 N1 为 `epoch_.fetch_add(1, seq_cst)`（记 R）。任一等待者 W 的 W5 复查若读自 R（或其后的 SC 修改），则 **E happens-before W 此后的一切操作**——特别地，W 的重扫探测必然读到 E。

证明：R 是 release RMW（seq_cst 蕴含），E 程序序先于 R（提交路径 PO）；W5 是 acquire load（seq_cst 蕴含）且读自 R ⟹ R synchronizes-with W5 ⟹ E happens-before W5 ⟹ E happens-before W5 之后（PO）的重扫探测；由 write-read coherence，重扫探测的槽位读只能取 E 或其后继写入的值 ✓。

**v2.1 的引理 1 证伪记录（P3-t）**：v2.1 断言"由 D0（seq_cst fence），E 在闸门判定前对全体线程可见；此后任何在 SC 序中位于闸门之后的读都能读到 E"。这在 C++ 内存模型下不成立：fence 的 synchronizes-with 只经**同一原子对象**上的读-写对（fence-fence / fence-op 配对）建立——fence 后面跟一个 SC load 并不会把 fence 之前的非 SC 写"发布"给在 S 中排在后面的其他线程读；"全体可见"是 x86 mfence 的硬件直觉，标准无此规则（与 v2.1 批判 v1 的"release store 参与 S"同型错误，抬高一层）。

**引理 2（二择：闸门读到注册，或等待者读自新 epoch）**：对任意提交 S（含 R、G 与可选 wake）与任意试图入睡的 worker W：要么 S 的闸门 G 读到 W 的注册（wake 路径生效），要么 W 的 W5 读自 R 或其后继（epoch 路径生效）。

证明（用 S 全序：waiters_/epoch_ 全部 seq_cst；R、G、W2（W 的 waiters_++ RMW）、W5 均在 S 中）：
- 由 D0'（R 先于 G 的 PO）⟹ S 中 R < G。
- 情形 A：G 读到 ≥ 1 ⟹ 存在注册 W2 在 S 中先于 G ⟹ wake 路径 ✓（引理 3 保证 wake 不会落空）。
- 情形 B：G 读到 0 ⟹（SC 读取语义：G 取 S 中其前最后一个 SC 修改的值）所有注册 W2 均在 S 中位于 G 之后 ⟹ 对每个 W：R < G < W2，且 W2 < W5（PO 延入 S）⟹ **R < W5 于 S** ⟹ W5 取 R 或其后继的值 ⟹ 引理 1 生效 ✓。
- 关键：B 分支的"R < W5"**依赖 R 先于 G**——若实现把 epoch 推进放在闸门之后（v1.0 顺序）或改为条件式，R < W5 不再必然，B 分支开口（v2.1 的"闸门读 0 而注册在途"窗口由此复活）。此即 D0' 的由来。

**引理 3（W5 复查 + W4 先行的窗口封闭，P0-1 修正的证明，保持）**：对"W5 读到旧 epoch、即将 W6 入睡"的窗口，任何已生效的 notify 都使 W 无法真正入睡：
- notify 的 N3 若发生在 W4 之前 ⟹ W6 的内核值比对必失败（睡眠字已被 N3 改写）⟹ EAGAIN 立即返回 ✓（此分支不依赖 W5 与 N1 的 S 序关系）
- notify 的 N3 若发生在 W4 之后 ⟹ N4 的 `futex_wake(1)` 要么直接唤醒已入队的 W，要么（W 尚未 W6）N3 已把睡眠字改为新值，W6 的内核值比对失败（EAGAIN）⟹ W 立即返回重扫 ✓
- v1 的缺陷正在于把"写睡眠字"放在复查之后：通知在复查后、写字前到达时，wake 空放且等待者随即写回旧值入睡。W4→W5→W6 顺序利用 futex 的**值比对**把该窗口双向封死。

**主定理（不丢唤醒）**：在 D0'–D4 下，任何入队任务都不会在所有 worker 入睡后被永远搁置。证明：任取任务 T（入队事件 E，其提交触发 notify S），取任意"检测遗漏 T 且将入睡"的 worker W：由引理 2，若在情形 A 则 wake 送达，W 醒来重扫（重扫或直接见 E，或经其后的 epoch RMW 与引理 1 迭代逼近——每个后续提交都是新的 S）；若在情形 B 则 W5 读自 R ⟹ 引理 1 ⟹ 重扫必见 E。若 W 仍入睡（E 未见于 W5 前）则 S 的 wake 由引理 3 保证不落空。终态：任务可见性要么直接成立、要么由某个提交的 R 锚定——不存在"全员入睡且无人可见"的静止点 ✓（与场景 #12 的虚假唤醒同一兜底）。

### 3.4 waiters_ 计数的一致性边界（修订 v1 的引理 3）

`waiters_` 为 SC 计数，语义是"当前处于 W2 与 W7 之间的 worker 数"（近似——因为 W3 命中者立即注销）。它被依赖的唯二判定：

1. N2 闸门（waiters_ > 0 才值得 wake）：其安全性由引理 2 保证，而非计数本身精确（P3-t 后闸门不再决定 epoch 推进）；
2. 观测（13 节的 wake 统计）：允许瞬时不自洽。

v1 用 relaxed 时，闸门读可能读到陈旧 0 —— SC 要求因此是正确性的硬条款，不是性能考量。

### 3.5 64 位 epoch 与 32 位睡眠字的截断（ABA 工程论证，P3-q 措辞收口；v1 结论保留）

危险等价于"某 worker 的 key 低 32 位与当前字相同、但实际 epoch 已前进 2³²"。窗口 = 该 worker 从 W1 到 W6 的时间（≥ 一次全量探测成本），需要窗口内发生 2³² 次 notify 才可能混淆——notify 次数受提交率限制，物理不可达。**P3-q 修订**：此为工程论证而非形式证明（2³² 边界无法在 soak 中实测到达），表述与门禁严格按"工程假设 + 运行时监护"处理：初版不做防范，debug 构建以断言记录该假设（写入 ASan 巡检项与 soak test）；若未来引入 notify 无提交率上限的来源（如定时器/管理通道），必须重新评估本条。

## 4. notify 语义细节（v2.4）

- **N0 fence 已废除（P3-t 证伪记录）**：v2.1 把 N0 写成"正确性条款（D0）"，论证是"把入队发布钉在闸门判定之前"。该论证在 C++ 内存模型下不成立（§3.3 证伪记录）——fence 只经同一对象上的读-写对建立同步，无法让"后续任意读"看到 fence 前的非 SC 发布；x86 上 mfence 的等效效果是架构巧合，不能作为跨平台协议的依据。**方向警示（P3-t，取代 P3-g 的旧警示）**：v2.1 推翻 v1"无条件 epoch++"的依据（"fence 更便宜且等价"）已被证伪——v1 的结构正确、坏的只是当时的证明；v2.3 以 D0' 收回无条件推进，并保留闸门为纯性能优化。任何人若再次以"性能"为由把 epoch 推进改为条件式、或把 fence 请回正确性位置，必须先重证 §3.3 引理 2 并在本表登记。
- **D0' 的成本**（P3-t 新增）：每次提交无条件执行 `epoch_` 行 seq_cst RMW（共享行写）——列入 benchmark-plan §8 陷阱清单观测项；候选降本方向（per-worker waker、条件式推进的"可证变体"）须先过 §3.3 重证，禁止先改后证。
- **快路径统一口径（P3-u）**：worker 内快路径入队同样无条件 `notify_one()`，且主文档 §8.4 的顺序纪律为 **push → notify → drain**：notify 必须先于 drain（drain 执行用户代码，若其阻塞等待刚入队的 child，被唤醒的窃取者必须已在路上；若换序而 drain 不返回，wake 永不到达）。drain 内重入提交产生的嵌套 notify 无害——唤醒者重扫幂等（§3.3 引理 3 / 场景 #12 兜底）。
- **drain 再入队批发布口径（P3-ad，P0-3 修复）**：`drain_spill_overflow`（主文档 5.3）把溢出项重新入全局队列同样是"D2' 一切入队点"的管辖对象——修订后的纪律为**批发布 + 一次 notify**：先完成本轮**所有**成功入队（期间只有入队与自旋，无用户代码无阻塞），随后一次 `notify_one()`（epoch RMW 一次覆盖全部发布，闸门决定 wake），**之后**才执行 fallback 的 CallerRuns 用户代码。反例：旧骨架逐项入队后不再 notify，溢出项既无 epoch 推进也无唤醒，其他 worker 全睡时该项长滞（fallback 任务再阻塞即实际停滞）。fallback 内重入提交自带快路径 notify，无需补发。
- `notify_all`（仅 shutdown）：无条件 `epoch++` + `futex_wake(INT_MAX)`，不走闸门（关停期唤醒的对象是已经在睡的 worker，wake 全发即可；epoch 先行与 D0' 同构，保证晚注册者 W5 拦截）。
- 极斜负载风险（v1 保留的观察点）：被唤醒的单一 worker 可能吃掉大批任务、其他 worker 继续沉睡。缓释手段："领走任务后若全局仍有剩余且 `waiters_ > 0` 再补一次 notify"——触发条件由 benchmark 数据裁决。

## 5. 共享 waker vs 每 worker waker 的裁决

### 5.1 选项

| 方案 | 结构 | 优点 | 缺点 |
|---|---|---|---|
| A. 全池共享（已选） | 1 个 EventCount | submit 侧定位单一；协议对象唯一 | 所有睡眠者共享 `waiters_`/`futex_word_` 缓存行；唤醒"任意一个"而非"最合适的一个" |
| B. 每 worker 一个 | N 个 EventCount，提交者按 hash(victim) 定向 notify | 睡眠/唤醒零共享；可定向唤醒 | submit 需选 waker（新增全局选择点）；shutdown 需 notify_all × N；前提 D0'/D1'/D2' 与 §3.3 二择证明要按 per-waker 全量重写 |

### 5.2 裁决与判据

- **初版选 A**：正确性前提在单一 waker 上表述清晰（D0'–D4）、实现面最小，符合"简单兜底"（upstream §2.1）。**P3-t 成本注**：共享行的写频率分两层——`epoch_` 行 = 提交率（D0' 无条件推进，与 pending_ 同列可测热点）；`waiters_`/`futex_word_` 行 = 实际睡眠/唤醒转换率（三级退避压低）。若 perf 显示 epoch_ 行成为 top 争用点，转 B 或立项"可证的条件式推进"——两者都要过 §3.3 重证。
- **切换判据（P2-2 修订 + P3-ad 口径修正：改用"真正被唤醒后的命中率"）**：
  1. `perf` 显示 `evc.waiters_`/`futex_word_` 行在 `bench_idle_cpu` 中成为 top 争用点；
  2. 内置指标 `post_wake_hit / (post_wake_hit + post_wake_empty)` 的**有效唤醒率** < 50%，且 p99 恶化可归因。**P3-ad 修正**：旧判据误用 `wakes_effective`——它测的是"入睡前 full_scan 是否找到工作"，不是"被 wake 之后是否找到工作"，与"是否该换 per-worker waker"无关；该语义已迁为 `pre_sleep_scan_hit/empty`（upstream §13），判据改用 post_wake 口径；`wake_threads`（FUTEX_WAKE 返回值累计）作为惊群/过量唤醒的第三观测项。
  两个指标由 futex 与 generic 两版实现共同填写（upstream §13.2），不需要外部探针。
- **转 B 的实现预置**：`WorkerCtx` 留 waker 指针槽位（当前统一指向共享实例），victim 选择切面（upstream §12）复用为"唤醒目标选择"；协议证明按 per-waker 重写 D1'/D2'。

## 6. futex 移植层

### 6.1 接口（实现文件各自独立，语义严格一致）

```cpp
// 内部 API（非公开头），thin wrapper：
void futex_wait(std::atomic<std::uint32_t>* addr, std::uint32_t val) noexcept;  // EAGAIN/EINTR 立即返回
void futex_wake(std::atomic<std::uint32_t>* addr, std::int32_t count) noexcept; // count = 1 或 INT_MAX
```

### 6.2 Linux 实现约束

- 直调 `syscall(SYS_futex, addr, FUTEX_WAIT, val, nullptr, nullptr, 0)`：
  - `EAGAIN`（值不符）与 `EINTR`（信号打断）**直接返回**——协议把两者都当虚假唤醒（引理 3/主定理的兜底），这是刻意决策而非疏漏；
  - 原子变量地址经 `reinterpret_cast` 传给 syscall（`std::atomic<std::uint32_t>` 布局与 `uint32_t` 兼容，内核只按地址读值，C++ 原子性不参与内核语义，但**用户态的并发写必须走原子**——P0-2）。
- 编译条件 `#if defined(__linux__)`。

### 6.3 generic（mutex+condvar）实现约束

- 同签名实现 enter/leave/wait_registered/notify_one/notify_all：`wait_registered` 的睡眠段 = 上锁 → W4 写字 → W5 复查（持锁状态下把"注册态"转入 condvar 等待）→ `cv.wait`；notify = 上锁 → epoch++ → 写字 → unlock → notify_one/all。W2/W7 的 waiters_ 计数同样维护（两版共享同一协议骨架，仅睡眠原语不同）。
- 定位声明（写入文件头注释）：**仅 CI/非 Linux 平台兜底**，性能不可作为验收数据来源；Linux 结论以 futex 版为准。
- **双实现同测（D4）是门禁**：TSan 不建模裸 futex 的 happens-before（可能漏报/误报），generic 版是协议正确性的可检测背书——任何 EventCount 改动必须两版同套测试通过。

## 7. 测试对应（v2 扩充）

| 测试（upstream §10.4） | 覆盖的性质 |
|---|---|
| `WakeupLossWindow` | 场景 #8：W4/W5/W6 窗口期压入 notify（引理 3） |
| `SpuriousWake` | 场景 #12：EAGAIN/字被覆盖后必须重扫 |
| `WakeupSuppressionStall` | P0-4 回归：全 worker 入睡后单次提交必须有界执行 |
| `DrainNotifyRace` | P0-6 回归：pending 1↔0 抖动时并发 drain 不挂死 |
| `DrainDuringShutdown` | 场景 #11：D3 纪律（关停期不睡） |
| soak（weekend 级） | §3.5 截断假设 + 长期运行无停滞（挂 ASan 断言） |

## 8. 后果与开放性

- **受益**：唤醒路径无内核 mutex、无惊群。**P3-ad 清理陈旧文字**：v2.3 起 notify 每次先无条件执行 `epoch_` 的 seq_cst RMW（共享行写），再按 `waiters_` 闸门决定是否 futex_wake——**"无等待者时退化为一次只读比较、不触碰共享可写行"是 v2.1 口径，已被 P3-t 证伪废止**，此处不得再作为实现依据（否则会经"省一行写"的优化把场景 #13 的 P0 重新引回来）。无等待者时真正省下的是 futex 字写与 syscall。
- **代价**：协议正确性前提（D1'–D4）外置于 EventCount——实现评审必须把这些纪律作为 checklist 项；**D1'（注册先行）是 v1 教训的核心，任何重构不得把它移回探测之后**。
- **开放项**：
  1. per-worker waker（§5.2 判据触发时立项）；
  2. 唤醒定向（优先唤醒低负载 victim）作为 B 方案内的可选增强；
  3. epoch 低 32 位回绕的 soak 验证（§7 最后一行）。