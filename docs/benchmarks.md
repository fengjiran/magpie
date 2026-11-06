# M2 benchmark 使用与证据边界

当前 harness 默认测 mutex/global/CV 基线，也可测 MPMC、Chase-Lev 和 M5 EventCount 候选。Linux/macOS 可构建；Windows 的 CPU clock/affinity 尚未实现，启用 benchmark 会明确报错，普通库构建不受影响。

## 构建与 smoke

```sh
cmake --preset bench
cmake --build --preset bench --parallel 2
python3 scripts/run_benchmarks.py suite --repeat 1 --smoke \
  --output results/bench-smoke-NEW
```

benchmark opt-in，生产库没有新增运行时依赖。Release 生产库与 harness 都通过现有 helper 使用 C++20、最终生效的 `-O2`、warnings-as-errors；不会给库添加 benchmark 专用 flags。`BUILD_TESTING=ON` 且 `MAGPIE_BUILD_BENCHMARKS=ON` 时另注册直方图、样本分类、守恒与 runner 归档校验测试。CI 仅编译、正式 fast 测试和短 smoke，不以共享 runner 的数字作性能验收。

`magpie_bench --help` 列出参数；一个可执行文件以 `--program` 分派下列程序。所有运行由独立子进程完成。任何已有输出目录都会被拒绝；失败日志保留，不自动重跑。本文命令中的 `results/` 归档只保留在本机，不随仓库跟踪。

| 程序 | 负载和解释 |
|---|---|
| bench_empty_task | 外部提交、空任务、Abort；worker/producer 数显式设置 |
| bench_submit_path | pool：完整 submit；queue：benchmark 自有受控 mutex 指针 ring，单 consumer、不执行用户代码；wrap：同线程 TaskImpl new/delete |
| bench_producer_scaling | 固定 workers，producer 1/2/4/8；CallerRuns 为独立配置 |
| bench_fine_grain | cpu：可标定 scalar mixing；memory：1 KiB immutable、cache-resident 读集合；atomic：有意共享的原子 payload；sleep：指定毫秒 |
| bench_burst | 单 driver 发起有限非阻塞树，depth 2/4/8/16、fanout 2；子项先发布，父项后完成；driver 在批间 drain，无 worker future 等待 |
| bench_latency | 相同负载/资源的饱和吞吐标定后，以固定 10/50/90% 到达率测 submit→任务 body 结束；同时报告发送延迟和未发出的到达项 |
| bench_idle_cpu | 指定 worker 空载固定窗口；process CPU / wall / worker 数，包含 driver 的极少量观测成本 |
| bench_baselines | pool、显式 launch::async、每 task 新线程；相同 CPU payload、每批 workers 项、最大 workers 个执行线程，批间全部完成再开始下一批 |
| bench_alloc_cost | same：同线程 TaskImpl new/delete；cross：producer 分配、单 consumer 删除；不可入队的包装由 producer 删除，两个销毁计数分开 |
| bench_shutdown_drain | 提交有限存量后先 shutdown，再释放工作门、drain、销毁/join；分别报告 wall 和 process CPU、所有任务 body 的 thread CPU |

M4 harness 新增 `bench_worker_scaling` 和 `bench_skew`，global 构建可作 worker-spawn 对照；M5 新增 `bench_notify_scan` 稀疏发布程序，CV 可作控制组。旧冻结 binary 的 suite 仍保留其版本边界。不能用外部 producer 数变化冒充本地 scaling；inline fallback 单列。

`queue` fixture 不是生产私有 ring 的直接入口，没有 Gate/pending/通知。`wrap`、`queue`、`pool` 的差异是受控对照，不能简单相减得出某个 RMW 的独立耗时。尚未对 gate/pending/submitted 单独硬件采样；Linux 可以另用 `--profile` 保留 perf stat 的 cycles/instructions/cache-misses/context-switches/cpu-migrations。profile 数据与无 profiler 的测量分组，不能拼接 A/B；不存在的 epoch/WaitSlot/slot seq 成本为不适用。

## 窗口、采样和 CPU 口径

每个进程先运行独立 warmup phase，再新建池和预分配记录进行测量。连续提交使用固定 steady_clock 截止时刻；吞吐分母从开始到 producer 退出、shutdown、drain 完成，包含排空尾部。`window_s` 是停止生产/有限批结束时刻，`elapsed_s` 是实际吞吐分母；不把截止时仍在队列中的项计入固定窗口完成量。burst 最后一棵树会越过截止时刻，实际 elapsed 如实记录。

生产 Task 包装仍每项分配；harness 的样本记录按 producer 预分配，每个完成任务只写自己独占的样本槽，drain 后才读取。没有 harness 自有的共享热吞吐计数器或每项记录分配。总量取池计数和 producer 本地计数，sample_stride 外的任务不调用任务侧计时；empty 路径直接提交无状态空 callable，payload 类型在 phase 初始化时解析，任务内不做字符串分派。采样项携带计时捕获，其包装大小与纯空项不同，所有对照须保持相同采样设置。CPU payload 的结果通过 compiler barrier 保留；记录 steady_clock 和循环截止检查本身有成本，所有对比必须使用相同 harness。

默认每 producer 65536 条记录，每 1024 次尝试采样一次；拒绝样本没有完成记录。存储耗尽会使 runner 失败，需要在新目录调整 stride/capacity，不能用截断样本宣称全窗口 p99。饱和时成功样本可能少于尝试样本，拒绝数单列。固定到达率使用从测量开始推算的绝对 deadline；落后后追赶，窗口关闭后不继续补发；`arrival_lag_*` 和 `missed_arrivals` 明示供给不足，submit latency 不包括未进入 submit 的排队时间。

64 个对数桶覆盖 1 μs–1 s，桶宽比约为 `10^(6/64)≈1.241`。既有 `*_us_upper` 列继续表示桶上界，underflow/overflow 计数保留。2026-11-05 起，新增 `p50_us/p99_us/p999_us` 及 worker/inline/arrival_lag 对应列，从全部有效样本按 nearest rank 计算，定义字段为 `nearest-rank-sample-v1`。聚合、排序和归档均在计时窗口之后执行，不增加热路径打点。无样本为 `-1`；精确表示对已收集样本精确，不消除采样和跨进程不确定性。

旧桶上界之比不能证明 5% p99 门槛，旧归档缺精确列时标记待补证。`compare_p99` 只比较各进程样本 p99 的中位数，不证明无新峰或统计置信界。runner 的 `bench_latency` 每进程另存 `run-XX.samples.tsv`（纳秒 latency/arrival lag、执行分类及 SHA-256），用于复核分布；旧 binary 不产生文件时 manifest 明示证据缺失。I/O 路径通过独立环境变量传递，不改变负载配置 hash；直接运行可用 `--samples-output PATH`。warmup 不写归档，已存在文件拒绝覆盖。

比较时样本数必须为正整数（十进制整数字符串或 int）；非有限值、小数、布尔值及不可解析数据返回 `insufficient-evidence`。p99 本身也须可解析、有限且非负。

原始 TSV 仍保存全部桶计数和执行分组。submit→body-end 早于 capture 析构和 pending 递减；shutdown latency 特指 task body wall。新口径 A/B 需用同一 harness 测量 mutex 和候选，另建冻结源码/binary/校验和，保留旧包，不能从旧聚合桶恢复精确分位数。

准确的 completion totals 在 producers 已 join、shutdown、drain 后校核 `pending=0`、`submitted=completed+discarded`、`completed=worker_completed+inline_completed`。M1 公开 API 不提供独立 join，因此最后 worker 的关闭通知可能仍在完成，`wakes` 不是严格最终通知总数；`wake_threads=NA`。M1 没有的 steal/scan 字段保持零，不能宣称这些路径被测量。`peak_threads_bound` 为结构上限，不冒充采样得到的峰值。

shutdown 的 setup、预分配和工作门等待不计入测量；warmup 至少达到指定时间。它是 benchmark-plan 规定的有限存量实验，`duration` 不是强行截断这些任务的 deadline，`window_kind=finite-inventory`；剩余程序使用固定时间窗口。存量 10^3/10^4、10/100 ms 在完整 suite 中保留，smoke 只用 32 项×0.1 ms。`task_cpu_s` 为任务 body 实际 thread CPU 累计，不能拿 sleep wall 当有效 CPU 工作。

`--shutdown-pattern` 支持 uniform（原均匀存量）、single-tail（仅最后一项长任务，允许 task-count=1）、mixed（每四项一项长任务，其余为其百分之一时长）、caller-runs。后者要求 rejection=caller-runs、task-count=capacity：先逐一占用所有 worker，再填全局队列，由额外 producer 内联执行最后任务；shutdown 等该提交离门后再释放工作门。其 task CPU 包含 shutdown 窗口的 inline body，不能直接用 drain_cpu−全部 task_cpu 解释纯忙等。

## 资源与完整矩阵

Linux 配置同时指定 `worker-cpus` 和 `producer-cpus`（逗号分隔、不得重复），只接受当前允许集合内的 CPU。创建池前将 driver 的 affinity 临时设为 worker 集合，Linux M1 worker 按该集合轮转绑核；创建后恢复，并将 driver/producer 按 producer 集合轮转绑定。显式 affinity 失败导致运行失败，M1 best-effort warning 会拒绝正式结果。过量线程或 worker/producer 集合重叠允许但必须记录并在对照中保持一致。async/每任务线程模型在每个新线程上设置 affinity，其 OS 配置成本计入该模型；池 worker 的创建/绑核在测量窗口之前完成，这一摊销差异必须作为模型成本解释。macOS 不模拟 CPU affinity，记录 unbound，数据只供开发。

```sh
# Linux: config.json 填写实测可用集合，例如 worker-cpus/producer-cpus。
python3 scripts/run_benchmarks.py suite --config config.json \
  --repeat 5 --platform-attested physical --output results/linux-m2-NEW
```

suite 保留原矩阵，并为支持新参数的 binary 增加 single-tail/mixed/caller-runs 关闭场景；完成 empty 后按 median worker throughput 标定同资源空任务 latency。`--platform-attested physical|wsl2` 需要操作者确认 CPU 集合及固定环境，WSL2 另核对虚拟化检测，登记见 benchmark-plan。平台资格不等于性能门槛已通过。

记录 CPU 型号/核/SMT、允许 CPU、cgroup v2 quota、NUMA/topology、可读取的频率策略、工具链和依赖；cgroup v1 quota 未解析时明确 unavailable，正式报告须补齐。Linux 最低工具链和 pthread backend 尚需 CI/原生结果。Linux 主机/CPU 配额/频率策略没有实际控制时，不凭脚本存在宣称资源一致。

## 冻结与重跑

```sh
python3 scripts/run_benchmarks.py freeze --config bench/configs/empty.json \
  --repeat 5 --output results/milestones/m2-baseline-NEW
python3 scripts/run_benchmarks.py replay \
  --baseline results/milestones/m2-baseline-NEW --repeat 5 \
  --output results/milestones/m2-replay-NEW
```

freeze 在临时 clone 中覆盖当前源码快照、生成独立 baseline commit 与完整 `source.bundle`；主工作区 HEAD/index 不变。results 不作为测量源码覆盖进快照，原工作区 revision/dirty 状态、逐文件源码 hash 另外登记。从该 commit fresh configure/build，Release fast/stress 通过后才复制静态链接生产库的 `magpie_bench`，归档 compile commands、依赖、binary/bundle SHA-256，然后运行指定配置。快照过程中若源码变化则冻结失败；失败目录保留。

本轮 M1 runtime 已有 macOS sanitizer 证据，M2 新 harness 的 sanitizer 检查另外归档；snapshot 的 Release 检查不替代平台/sanitizer 验收，也不关闭已有测试诊断缺口。

replay 在执行前校核 binary、bundle、初次 raw TSV 和配置身份，使用冻结配置；不重新编译，不覆盖初次历史。每组默认≥5个独立进程、warmup≥2 s、固定时间程序 duration≥10 s；`--smoke` 明确降级。原始 TSV、命令/exit code/stdout/stderr、环境、median/Q1/Q3/IQR、同日/资源一致性和 repeatability ratio 都保留。formal A/B 还要求同日、同资源、物理 Linux，且无 profiler；重复性比值本身不是优化 speedup。

若初始 manifest 已登记 `runs[].samples_sha256`，replay 还会在执行前逐一检查对应原始样本文件及校验和，缺失或损坏直接失败，不创建新的测量输出。旧版未登记原始样本的归档继续按其聚合数据范围处理，不能据此追认分布证据。

replay 的 `--smoke` 保留冻结配置和窗口，但无论 repeat/warmup/duration 是否达到数值要求，正式平台与 A/B 资格都为 false。初始归档若标记 smoke，即使旧 runner 曾错误写入 eligible=true，也不能成为正式对照来源。

可恢复源码：

```sh
git clone results/milestones/m2-baseline-NEW/source.bundle /tmp/magpie-baseline
# 如 bundle 的 HEAD 未自动检出，按 baseline.json 的 baseline_commit checkout。
```

M2 只冻结 correctness 基线与测量机制，不关闭主设计的六项性能门槛；正式报告继续按 benchmark-plan 五问解释数据和成本转移。

## M3 候选对照

`bench_submit_path --mode mpmc` 测量真实 header MPMC 原语，载荷为同一个有效 borrowed marker，单 consumer、producer 供给、无用户任务执行；`--mode queue` 是对应 mutex fixture。两端使用相同模式，完成后逐项守恒检查，CTest 有单独 routing smoke。capacity 是同样的消息槽数，MPMC Slot 含 seq/data（Task* 时16 bytes），mutex 指针槽8 bytes，`queue_slot_bytes` 明确记录额外 footprint。

完整池只由库的编译 backend 决定，`global_queue_backend()` 给 TSV 的 `generic/pthread-mutex/mpmc-cv` 标签；旧 M2 frozen binary 的 mutex 标签保持不变。同机器/config 的原 binary 复跑和候选运行必须校核各自 checksum、commit/source identity。两种弱/强 unavailable 频率会影响拒绝数，应同时报告 attempts/accepted/refused 和 worker/inline，不按逻辑深度推导 full。

M3 仍需通知 mutex 与 gate/pending/submitted/alloc，没有 epoch。本机对照只用于机制研究，固定物理 Linux/完整资源与矩阵缺口仍然存在。具体数据、失败现场与五问见 [M3 milestone](milestones/M3.md)。

## M4 本地路径

使用 `bench-workstealing` preset。`--local-capacity N` 记录真实每worker槽数，默认16；`--spawn-pattern uniform|hotspot`控制worker根任务分布，不把外部producer变化当热点。worker scaling与skew使用固定`--task-count`子项批，外部Abort；只fixture root等driver释放，子项不等待future。root数量、leaf数量、worker叶吞吐、inline比例与成功窃取分别输出。准备/排空开销包含在测量窗口，driver CPU单列，worker CPU为process减driver的近似；这两个fixture无latency采样，p99为NA，必须另跑latency负载。

新版suite通过binary的`--help`检测新参数，增加worker1/2/4/8及两个skew smoke；旧冻结binary不接收新参数。runner只在配置明确出现新key时传递，保持M2/M3归档的checksum replay可用。开发代表点、buffer差异及正式门禁见 [M4 milestone](milestones/M4.md)。

## M5 停车机制对照

使用 `bench-eventcount` 构建候选，以相同源码、MPMC/work-stealing/CV 构建 control。除停车和相应退避外，worker/producer、buffer、payload、采样和到达率保持一致。每组至少5个独立进程、2s warmup、10s测量；shutdown/drain 仍用有限存量窗口。macOS 为 generic 开发数据，不能代表 futex/物理 Linux。

`bench_notify_scan --arrival-rate N` 复用实际池稀疏提交，输出 epoch、扫描总槽数、backend wake/return、注册数和 submit→body-end 的分组直方图。它不强制恰有0/1/N个注册者，也不隔离每个 RMW 的成本；读数是完整池的合计。比较 wakes 时注意 `wake_count_scope`：CV为notify请求，EventCount为成功标记后的backend调用。Linux才有实际wake_threads及wait_syscalls/EAGAIN/EINTR；generic输出NA。

M5 历史版本关闭期持续探测；2026-11-05 起以最终 pending/gate 通知支持关停停车。须同时报告 shutdown/drain/join 的 wall、process CPU 和 task CPU，不能以 sleep wall 当有效 CPU。新对照不得覆盖旧 binary 或失败，见 [M6 修复记录](milestones/M6-review-fixes.md)；原生历史证据见 [M5 milestone](milestones/M5.md)。
