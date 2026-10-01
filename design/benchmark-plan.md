# benchmark-plan：负载矩阵、基线与判读流程

- 状态：**生效中**（数据随阶段推进回填）
- 日期：2026-09-21；修订 2026-09-29（P3-q 批次）：§3 增补 bench_shutdown_drain；§8 增补全屏障（mfence）代价归因、external_stats_ 争用、布局对照实验（W2.9）三条观测/对照项；§7 挂钩 W2.8/W2.9。修订 2026-09-30（P3-u 批次）：§3 bench_burst 标注非阻塞前提与快路径通知必采项；§7 挂钩 W2.10；§8 增补陷阱 12（快路径通知代价观测）。修订 2026-10-01（P3-z 批次）：§1/§3 基线纪律重写——冻结对象为 commit+binary+历史数据，正式 A/B 同日重跑基线 binary；§4 加对比纪律总括；§7 W2.2 验收改"同日重跑基线"；§8 增补陷阱 13（环境漂移）。修订 2026-10-01（P3-aa 批次）：§3 六个程序挂 §14.1 数值门槛；§7 W2.6 验收改"数值门槛表六项全部达标"。修订 2026-10-01（P3-ad 批次）：§3 bench_empty_task 双向轴（producer × worker_count）与 worker scaling 阈值；bench_baselines 固定 `std::launch::async`；TSV 字段随设计 §13 重整（post_wake_*/pre_sleep_scan_*/wake_threads）
- 上位约束：[magpie设计方案.md](magpie设计方案.md) §13（内置指标）、§14（验收基准）、§16.2（常量标定）

---

## 1. 目的与总原则

1. 为设计 §14.1 的四条核心指标提供可复现的测量程序；
2. 为设计 §14.2 的"五问"提供数据来源——**每组 benchmark 都必须产出一份五问记录**，无五问即不可发布结论（设计 §14.4）；
3. 为 §16.2 的每个可标定常量（`BULK_LIMIT` / `SPIN_LIMIT` / `YIELD_LIMIT` / `VICTIM_TRIES` / `STEAL_CAP`）提供标定实验方案；
4. 建立**常驻基线**（阶段一 mutex 版）：冻结对象 = 阶段一**冻结 commit + 归档 binary（含校验和）+ 原始历史数据**（`results/baseline/stage1.tsv`），三者永不覆盖；后续所有变更与**当日重跑的基线 binary** 对比（P3-z，见 §3 对比纪律）。

## 2. 环境与机器纪律

| 项 | 要求 |
|---|---|
| CPU | 固定一台机器/一组机器，禁用 CPU 频率漂移：Linux `governor=performance`、关闭 turboboost 影响（或记录并归一） |
| 隔离 | `taskset -c 0..N-1` 绑核运行；worker `pin_to_cores=true`。**超线程说明**：设计 §12 默认取逻辑核、`pin_to_cores` 时按物理核绑定，基准须记录"物理核/逻辑核"配置并在结论中声明 |
| 运行环境 | 最终验收数据必须在**物理 Linux** 采集；WSL2 可作为开发期快速反馈（时钟与核调度有偏差，仅供趋势，不得作为 §14.1 结论依据） |
| 验收物理机 | 责任人/时间点【待定】——未就绪前设计方案 §14.1 的四条验收指标不可关闭 |
| 噪声 | 每配置 ≥ 5 次独立进程运行，报告中位数与 IQR；预热 ≥2s 后进入固定时长 ≥10s 的测量窗口（任务数只作下限，P2-13） |
| 共享计数行 | pending_ 与 counters_（submitted/rejected/discarded）两行都是 bench_empty_task 的 perf 并行采样对象（设计方案 P1-4/P2-H 决议）；P3-q 增补：external_stats_（CallerRuns 高发期的共享行，设计 §13）；P3-t 增补：epoch_ 行（notify 无条件 SC RMW，ADR-002 v2.3）与 pop 的 seq_cst fence 指令归因，同为 bench_empty_task 的 perf 必采对象（v2.1 的 D0 fence 已废除、不再采样） |
| 干扰 | 关闭无关服务；同机对比必须在同一时段完成（对比组间不跨天） |

## 3. benchmark 程序清单（按 §14.3 展开）

统一 harness 输出格式（自研轻量 harness，理由见 §6）：TSV 一行一配置，含 `name, config, ops/s, avg_us, p50_us, p99_us, p999_us, idle_cpu_pct, submitted, completed, rejected, discarded, stolen, local_spills, wakes, wake_threads, post_wake_hit, post_wake_empty, pre_sleep_scan_hit, pre_sleep_scan_empty, pending`（P3-ad：wakes_effective/wake_empty_scans 按设计 §13 重整为 post_wake_* 与 pre_sleep_scan_*，并新增 wake_threads）。

| 程序 | 负载 | 必测配置 | 回答的指标 |
|---|---|---|---|
| `bench_empty_task` | 空 lambda 提交 | **P3-ad：双向轴——producer 1/2/4/8 × task 10⁶；且 worker_count 1/2/4/8（提交端供给充足：生产者多开，队列不空）** | 空任务吞吐、单核 ops/s 上限、多生产者扩展性。**P3-aa/P3-ad 阈值**：单核 ≥ 1.0×10⁶ ops/s；**worker scaling T8/T1 ≥ 6.4×（§14.1 #2，测池的执行扩展性）**；producer scaling 无本表阈值，按 §8 陷阱 8 判据（8 生产者反降 ⟹ 全局队列分片立项） |
| `bench_fine_grain` | 原子自增 / 随机访存 1KB | 同上 | 真实任务粒度下的吞吐与缓存效应 |
| `bench_skew` | 单热点偏斜 + 深队列突发（单生产者狂发） | 热点生产者 1 × 任务粒度两级 | 窃取生效性（`stolen`）、偏斜下吞吐损失、p99。**P3-aa 阈值**：单热点 vs 均匀吞吐损失 ≤ 20%（§14.1 #3） |
| `bench_burst` | fork/join 嵌套（任务内再 submit，**非阻塞**——worker 内等待池内 future 为禁令，设计 6.4/P3-u） | 深度 2/4/8/16（16 项覆盖极端递归） | 本地快路径（`local_spills`）、嵌套开销、快路径通知的唤醒数与 epoch_ 行争用（P3-u 必采） |
| `bench_latency` | 提交→完成时延直方图 | 固定提交速率阶梯（10%/50%/90% 饱和） | p50/p99/p999、唤醒质量（`wakes` vs 有效唤醒）。**P3-aa 阈值**：各负载形态 p99 ≤ 同日重跑基线 binary 的 1.05×、直方图无新峰（§14.1 #4） |
| `bench_idle_cpu` | 无任务空转 10s | — | 空载 CPU（三级退避是否生效）。**P3-aa 阈值**：≤ 3%/worker（§14.1 #5） |
| `bench_baselines` | 同负载跑 `std::async(std::launch::async, …)`（**P3-ad：launch 必须显式固定**——默认 launch 是实现定义的 async/deferred 二选一，不固定则跨 libstdc++/libc++ 基线不可复现）/ 每任务一线程 / 阶段一 mutex 版 | 与上同配 | 对比基线。**P3-aa 阈值**：vs `std::async` ≥ 2×、vs 每任务一线程 ≥ 10×（§14.1 #6，按 §3 同日重跑纪律） |
| `bench_alloc_cost` | 同一任务密度 | 裸 `new`/`delete`、池内分配、mimalloc-override | 回答"每任务一次堆分配是否触碰 10⁶ ops/s 的预算" |
| `bench_shutdown_drain` | 长任务（ms 级）+ 随机时点 shutdown/drain/析构 | 任务粒度 10ms/100ms × 存量 10³/10⁴ | 关停窗口忙等 CPU·秒（W2.8 的触发判据，设计 §9.1 语义其二）；join 有界性回归 |

对比纪律（P3-z 重写）：任何"两版对比"必须**同程序同配置同日采集**（§2 环境纪律的"对比组间不跨天"同源）。**"冻结基线"与"同日对比"的协调**：

- 阶段一 mutex 版冻结的是 **commit + binary（归档至 `results/baseline/bin/`，附 sha256 校验和）+ 原始数据 `stage1.tsv`**——三者永不重测覆盖，`stage1.tsv` 是当天的历史留档；
- 任何"与基线对比"的正式 A/B（如 W2.2），**当天重跑被冻结的 baseline binary**，与新版同窗采集再对比——历史 `stage1.tsv` 仅作长期趋势参考，**不得作为回归/提升判定依据**（环境漂移会让跨日对比产生假信号，见 §8 陷阱 13）；
- 基线 binary 缺失/校验失败即视为基线不可用，该对比作废——归档纪律本身是验收前置条件。

## 4. 判定与五问记录模板

每组 benchmark 产出文件 `results/NAME_YYYYMMDD.md`，内容强制包含：

```markdown
# benchmark：<name>（配置 hash：<git sha 前 7 位>）
## 数据摘要
- 吞吐 / p50/p99/p999 / 关键内置指标（贴 TSV 摘要行）
- 对比对象与差异百分比
## 五问
1. 哪条路径变快了？为什么变快？
2. 成本是否只是转移到全局队列或别的 worker？   ← 必须引用 stolen / local_spills / 全局队列深度采样
3. 是否伤害了 p99 尾延迟？                     ← 必须给出直方图对比
4. 是否只在单线程/均匀负载下有效？             ← 必须含 bench_skew 数据
5. 偏斜负载下扩展性是否仍然成立？
## 结论与后续动作
（回退 / 接受 / 标定常数 / 立项新 ADR 四选一）
```

**硬性规定**：五问中任意一问回答不出，该组 benchmark 视为未完成，结论不得进入 ADR 或常数基线的变更依据。模板中所有"对比对象"严格受 §3 对比纪律约束——尤其与基线的对比必须同日重跑基线 binary（P3-z），历史数据仅作趋势参考。

## 5. 常量标定实验（§16.2 的"标定方式"落地）

| 常量 | 自变量扫描 | 采集程序 | 判据 | 预期动作 |
|---|---|---|---|---|
| `BULK_LIMIT` | 1/4/16/32/64/128 | `bench_empty_task` + `bench_skew`（深队列项） | 深层时 p99 与队列滞留不再改善的拐点；均匀负载吞吐无明显回退 | 回填 §16.2，附数据链接 |
| `STEAL_CAP` | 16/32/64/128/256 | `bench_skew` | 偏斜收敛速度（提交→完成均值的下降斜率）vs 窃取 CAS 争用 | 同上 |
| `SPIN_LIMIT` | 0/16/64/256/1024 | `bench_idle_cpu` + `bench_latency`（低负载阶梯） | 每 worker 空载 CPU 占比 ≤ 3%（或总增量 ≤ N×3%），首任务唤醒时延不恶化——按 worker 数归一（P2-13） | 同上 |
| `YIELD_LIMIT` | 0/4/8/16 | `bench_idle_cpu` + `bench_latency` | 同上（自旋后的让出深度） | 同上 |
| `VICTIM_TRIES` | 扫描 1/2/4/全扫描 | `bench_skew` | 偏斜收敛时间（首个任务提交到 `stolen` 指标达稳定值的时延）最小且吞吐无退化 | 同上 |

每次标定同样走 §4 的五问模板（标定也属于"变更"）；常数最终值的采纳条件是"至少两个基准程序（吞吐 + 尾延迟各一）无退化"。

## 6. 工具选型：自研轻量 harness（决策与理由）

- **不采用 google/benchmark**：需要提交→完成时延直方图、内置指标（§13 Stats）逐组 dump、以及"多生产者 + 提交速率阶梯"这类非纯吞吐场景；google/benchmark 的迭代模型会掩盖"速率受控"下的排队行为。
- **harness 规格**：~200 行；随机种子可复现；每配置重复数、warmup、速率阶梯均为命令行参数；输出 §3 的 TSV；**统计只依赖池的 `stats()`**（设计 §13 的可观测性条款）。
- 时延计测：提交侧打时间戳（`std::chrono::steady_clock`），任务内打完成时间戳，写入每任务 preallocated 环形记录（避免测量本身的堆分配污染）；直方图用固定桶（对数刻度 1μs~1s，64 桶）。

## 7. 阶段门禁挂钩

| 数据 | 隶属阶段（§16.3 / §16.4 工作项） | 验收 |
|---|---|---|
| `stage1.tsv` 基线 + 三策略语义基准 | W1.7 | 数据入库即完成 |
| MPMC 替换五问报告 | W2.2 | 与**同日重跑的 stage1 基线 binary**（§3，P3-z）对比，多生产者扩展性必须实现提升；历史 `stage1.tsv` 不作判定依据 |
| deque/窃取偏斜五问报告 | W2.4 | `stolen > 0` 且偏斜下 p99 改善 |
| EventCount/退避五问报告 + 常量标定 | W2.6 | **§14.1 数值门槛表（主设计 14.1，P3-aa）六项全部达标**（p99 项按同日重跑基线纪律，§3）；且通过双实现同测（ADR-002 §6.3） |
| 内存序放宽逐条数据 | W2.7（chase-lev-deque-adr §5.3 门禁） | 每处放宽附 TSan 与基准双证据；含 ARM64 实测（设计 §10.4 平台局限，P3-q） |
| 布局对照实验五问报告 | W2.9（设计 §7.1.1/§16.4，P3-q） | 每槽一行 vs 紧凑布局缓存 miss 数据，"防伪共享"先验判断被数据替代 |
| 关停窗口忙等数据 | W2.8（设计 §9.1 语义其二，P3-q） | bench_shutdown_drain 触发判据数据入库 |
| 快路径通知代价数据 | W2.10（设计 §16.4/§7.3.3，P3-u） | bench_burst 的 epoch_ 争用与唤醒转化数据入库；任何轻量变体须附场景 #19 重证 |

## 8. 已知陷阱清单（benchmark 侧）

1. **WSL2 时钟/调度偏差**：开发期数据不可作验收（§2）；
2. **提交端成为瓶颈**：多生产者基准里要测量"纯提交成本"（无池的裸 MPMC 入队）作对照，防止把生产者自限误判为池的能力；
3. **任务内计时的缓存污染**：时延记录的写与任务 caches 相互污染，必要时交替布局（记录数组与任务数据隔离缓存行）；
4. **频率漂移**：不接受不锁频数据（§2 的 performance governor）；
5. **初次运行偏差**：COW 页/分页影响，必须 warmup 后计时；
6. **`stats()` 快照不自洽**（设计 §13 已声明）：报告中只引用"稳态窗口末的快照差"，不引用瞬时值。
7. **共享计数行争用**：`pending` 是全系统最热缓存行，若 perf 显示其争用占比显著，立项 per-worker 计数重设计（设计方案 P1-4 开放项）；
8. **全局队列扩展性判据**：`bench_empty_task` 在 8 生产者下吞吐相对 4 生产者不升反降 ⟹ 触发"全局队列 N 路分片"立项（设计方案 16.3）。
9. **提交唤醒路径代价归因（P3-q 增补；P3-t 修订）**：P3-t 起 submit→notify_one 的 D0 fence 已废除，唤醒路径代价变为**无条件 `epoch_` 行 seq_cst RMW 共享行写**（ADR-002 v2.3），与 `pending_`/`counters_` 同列提交热路径；执行侧 pop 的 seq_cst fence（ADR-001 §5.2）仍每任务一次。bench_empty_task 的 perf 报告必须单列：pop fence 指令采样占比 + `epoch_` 行争用采样；若 epoch_ 行或 fence 合计 > 5%，依次排入 W2.7 验证序列或触发"per-worker waker / 可证的条件式推进"立项（后两者必须先过 ADR-002 §3.3 二择重证，禁止先改后证）；
10. **external_stats_ 争用（P3-q 增补）**：bench_latency 高饱和阶梯（90%）下 CallerRuns 高发，perf 采样 external_stats_ 行争用；占比显著则触发设计 §13 的 TLS 统计演进立项；
11. **模糊结论防词（P3-q 增补）**：布局对照（W2.9）的报告禁止出现"伪共享通常很少发生"之类先验表述——只有 cache-miss 计数与吞吐/p99 数据允许作为裁决依据（呼应设计 §2.1 简单兜底原则）。
12. **快路径通知代价观测（P3-u 增补）**：worker 内快路径每 child 一次 notify（场景 #19 修复），bench_burst 必须单列：唤醒数（wakes 与 stolen 的相关性）、epoch_ 行争用、嵌套深度 16 下"spawn 通知 vs 实际被窃"的转化率；若通知的共享行成本被坐实，W2.10 轻量变体（空→非空 / 仅 waiters_>0）按设计 16.4 立项——但必须以"场景 #19 仍闭合"的重证为前提，禁止只凭数据开枪。
13. **环境漂移（P3-z 增补）**：跨日对比受温度/功耗墙降频、microcode、kernel 调度、编译器与依赖库变更污染——漂移量级足以淹没 mutex 基线 vs 无锁版的 10–30% 目标差异。两条反模式：①拿冻结的历史 `stage1.tsv` 直接判回归/提升；②基线 binary 未归档导致无法同日重跑。纪律：→ 判定必须基于**同日重跑的基线 binary**（§3）；→ 归档路径 `results/baseline/bin/` + sha256 校验和是 W1.7 交付物的一部分，缺一即基线不可用。