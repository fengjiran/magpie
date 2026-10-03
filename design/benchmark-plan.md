# benchmark-plan：资源固定、路径分离与性能归因

- 状态：**2026-10-03 审核修订版，暂无实测数据**。
- 上位：[magpie设计方案.md](magpie设计方案.md) §13/§14/§16。
- 修订：区分worker执行、本地spawn、外部全局提交及CallerRuns；采集提交门、epoch和WaitSlot扫描；删除半批overflow/TSC相关观测。

## 1. 原则与基线

性能是待证目标，原语正确性与benchmark验收分开。每组结果回答主设计§14.2五问。

冻结mutex基线commit+归档binary+sha256+原始历史TSV。正式A/B同日重跑冻结binary，不覆盖旧数据；缺binary或checksum失败则对比不可用。变更拒绝/关闭语义后，先统一基线API语义再冻结，不能拿不同语义池比较速度。

## 2. 环境与资源

| 项 | 要求 |
|---|---|
| 机器 | 固定物理Linux；WSL/macOS仅开发趋势，不能作为最终futex验收 |
| CPU | 记录CPU型号、核/SMT、允许CPU集合、容器quota、NUMA、频率策略 |
| 绑核 | driver/producer和worker使用明确集合；比较版本使用同一集合 |
| 资源 | 对比固定producer数及CPU预算；CallerRuns不能额外计入worker scaling |
| 工具链 | 编译器/标准库/优化/宏/后端相同；不跨版本拼接数据 |
| 重复 | 每配置≥5独立进程，预热≥2s、测量≥10s，中位数/IQR |
| 归因 | gate、pending、submitted、epoch、waiters、cursor、扫描槽、slot seq、alloc均采样 |

最终物理机尚待安排；没有数据时不关闭主设计§14.1门禁。

## 3. 路径与程序

| 程序 | 固定条件与负载 | 测量解释 |
|---|---|---|
| bench_empty_task | 外部全局提交，Abort，无CallerRuns，固定producer数量/CPU集合 | 单worker空任务吞吐及端到端开销，不自动代表本地执行成本 |
| bench_submit_path | 不执行用户任务的受控纯队列对照、Task包装及完整submit分解 | producer供给、allocation、MPMC/gate/pending/epoch成本；受控队列测试不作为池吞吐 |
| bench_worker_scaling | 一个固定driver启动有限非阻塞任务树/批次，本地spawn+steal；worker1/2/4/8，固定工作总量与策略 | 只计队列获取后worker_completed；inline_completed单列；报告driver与worker运行CPU |
| bench_producer_scaling | worker固定，producer1/2/4/8，记录所有执行资源 | 全局入口扩展性；CallerRuns配置单独一组，不能与Abort组混算 |
| bench_fine_grain | 原子自增/1KB访存/可标定CPU工作 | execution成本、cache/带宽，不用共享原子递增假装独立任务线性扩展 |
| bench_skew | 单worker生产热点、均匀本地生产分别受控；相同总工作/线程预算 | 单热点vs均匀吞吐损失≤20%，不把“外部单producer”自动认定为单victim热点 |
| bench_burst | 有限非阻塞任务树，深度2/4/8/16，报告fanout与总项数 | 本地快路径、满队列转投、inline递归及栈开销；不阻塞等future |
| bench_latency | 固定到达速率10/50/90%饱和点、任务实际完成时间 | 提交→完成p50/p99/p999；按worker/inline分组并报告总体 |
| bench_idle_cpu | 无任务10s，worker固定集合 | ≤3%/worker，报告总CPU与归一方式 |
| bench_baselines | 同资源mutex池/launch::async/每任务一线程 | async固定launch策略；每模型峰值线程数与OS开销报告 |
| bench_alloc_cost | 同语义new/delete、候选arena或mimalloc对照 | 分配在producer、释放在worker的跨线程成本；候选不自动加入生产依赖 |
| bench_shutdown_drain | 10ms/100ms有限任务×存量10^3/10^4 | shutdown/drain/退出各窗口，CPU·秒及存量执行工作；不称硬时间保证 |
| bench_notify_scan | 注册者0/1/N，不同通知并发度 | epoch RMW、cursor、扫描长度、signaled命中与syscall费用 |

STEAL_CAP表示重复单项steal次数上限，不表示一次CAS取得半批。全局bulk仍每元素CAS；不能把减少循环入口写成每项同步已摊薄。

worker scaling使用worker_completed而非completed。若inline占比显著，标记测试受fallback干扰、调整本地容量/有限批规模并重测；不能按worker数给内联完成任务换个名字。报告原始比例，无预设“接近线性”结论。

## 4. 输出与统计

TSV至少包含：程序/config hash、producer/worker数和CPU集合、task粒度、rejection、backend、ops/s(worker/inline/total分别)、avg/p50/p99/p999、idle CPU、submitted/completed/worker_completed/inline_completed/rejected/discarded/pending/stolen/local_spills/wakes/wake_threads/post_wake_hit/post_wake_empty/pre_sleep_scan_hit/pre_sleep_scan_empty/notify_scan_slots。

generic wake_threads输出NA，不用1模拟实际唤醒线程数。post_wake_*包含backend的EAGAIN/EINTR/spurious返回，不命名为内核有效唤醒率。syscall返回累计、syscall次数和任务命中分开解释。

stats逐项快照可能不自洽；稳态差分须说明采样边界，精确总量由停止生产+shutdown+drain后的静止快照校核。时延在提交前用steady_clock打点，任务完成写预分配记录；未来就绪时刻与capture销毁完成时刻可不同，报告选定定义。

## 5. 门槛与标定

主设计§14.1六项为当前数值门槛：单worker≥10^6/s、独立worker scaling≥6.4、skew损失≤20%、p99≤同日基线1.05×、空载≤3%/worker、async≥2×/每任务线程≥10×。机器差异修订必须登记理由及双侧数据。

BULK_LIMIT {1,4,16,32,64,128}、STEAL_CAP {1,4,16,32,64,128}、SPIN {0,16,64,256,1024}、YIELD {0,4,8,16}、主循环VICTIM_TRIES {1,2,4}。所有接收量钳制local_capacity，入睡前全victim扫描不删减。

没有LOCAL_SPIN_US/tsc_now标定。通知优化只能在ADR-002完整重证后进入对照组。每项同时看吞吐与p99，不只挑一个有利负载。

## 6. harness

自研轻量harness支持warmup、duration、seed、repeat、到达速率、资源集合；不把“约200行”当实现限制。固定桶时延直方图（1μs~1s，对数64桶）应报告分辨率/溢出桶，不声称比桶宽更精确的p99。

避免benchmark自己共享一个热原子吞吐计数器；任务记录按线程预分配、末端聚合。driver不能隐式帮执行future任务。任务规模足够且测量窗口固定，短样本不作结论。

## 7. 阶段证据

W1.7冻结基线；W2.2提交/producer路径；W2.4worker scaling/skew/fallback；W2.6六项与WaitSlot扫描；W2.7每条内存序放宽独立数据与模型/ARM64；W2.9槽布局对照；W2.10通知成本与重证；W2.8关闭CPU浪费。

关停停车候选触发：退出窗口纯忙等CPU·秒大于任务有效CPU·秒的50%。将sleep wall time当有效CPU工作会错误归因，因此必须同时报告任务thread CPU time与wall time。

## 8. 易错解释

- 多producer提供更多CPU会让CallerRuns吞吐上升，不代表worker池扩展性。
- 外部入口的每个任务仍经过全局队列，不是只承担溢出。
- pending每个完成任务均共享RMW；gate每个submit至少两次；epoch每次通知都写。
- 有等待者时最多O(N)状态扫描，独立futex字没有消除提交侧共享epoch。
- 全局position差不是精确已发布深度；reservation hole尾延迟需要确定性测试，不靠自然benchmark覆盖。
- 无任务未必必须依次yield：参数收益依平台调度器，不能先验认定三级退避最好。
- 缓存行/slot布局按目标cache容量与争用衡量，不把每槽一行称作必然更快。
- 内存序、allocator、NUMA、分片等改变若无同日/同资源数据，不发布性能结论。

## 9. 结果模板

每组记录环境、原始TSV、基线binary checksum、资源和语义一致性、五问的机制解释，以及接受/回退/标定/立项结论。未完成正确性前置的变体不得作为可发布性能版本。
