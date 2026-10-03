# M2 benchmark 使用与证据边界

当前 harness 测量固定 worker + 全局有界 mutex ring + condition_variable 基线。它没有修改生产 ThreadPool，也没有接入 MPMC、Chase-Lev 或 EventCount。Linux/macOS 可构建；Windows 的 CPU clock/affinity 尚未实现，启用 benchmark 会明确报错，普通库构建不受影响。

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

`bench_worker_scaling`、`bench_skew`、`bench_notify_scan` 在 M1 不适用，suite 清单明确登记原因；不能用外部 producer 数变化冒充本地 spawn/steal scaling。树中的 inline fallback 计入 inline_completed，不计入 worker_completed。

`queue` fixture 不是生产私有 ring 的直接入口，没有 Gate/pending/通知。`wrap`、`queue`、`pool` 的差异是受控对照，不能简单相减得出某个 RMW 的独立耗时。尚未对 gate/pending/submitted 单独硬件采样；Linux 可以另用 `--profile` 保留 perf stat 的 cycles/instructions/cache-misses/context-switches/cpu-migrations。profile 数据与无 profiler 的测量分组，不能拼接 A/B；不存在的 epoch/WaitSlot/slot seq 成本为不适用。

## 窗口、采样和 CPU 口径

每个进程先运行独立 warmup phase，再新建池和预分配记录进行测量。连续提交使用固定 steady_clock 截止时刻；吞吐分母从开始到 producer 退出、shutdown、drain 完成，包含排空尾部。`window_s` 是停止生产/有限批结束时刻，`elapsed_s` 是实际吞吐分母；不把截止时仍在队列中的项计入固定窗口完成量。burst 最后一棵树会越过截止时刻，实际 elapsed 如实记录。

生产 Task 包装仍每项分配；harness 的样本记录按 producer 预分配，每个完成任务只写自己独占的样本槽，drain 后才读取。没有 harness 自有的共享热吞吐计数器或每项记录分配。总量取池计数和 producer 本地计数，sample_stride 外的任务不调用任务侧计时；empty 路径直接提交无状态空 callable，payload 类型在 phase 初始化时解析，任务内不做字符串分派。采样项携带计时捕获，其包装大小与纯空项不同，所有对照须保持相同采样设置。CPU payload 的结果通过 compiler barrier 保留；记录 steady_clock 和循环截止检查本身有成本，所有对比必须使用相同 harness。

默认每 producer 65536 条记录，每 1024 次尝试采样一次；拒绝样本没有完成记录。存储耗尽会使 runner 失败，需要在新目录调整 stride/capacity，不能用截断样本宣称全窗口 p99。饱和时成功样本可能少于尝试样本，拒绝数单列。固定到达率使用从测量开始推算的绝对 deadline；落后后追赶，窗口关闭后不继续补发；`arrival_lag_*` 和 `missed_arrivals` 明示供给不足，submit latency 不包括未进入 submit 的排队时间。

64 个对数桶覆盖 1 μs–1 s，桶宽比约为 `10^(6/64)≈1.241`。p50/p99/p999 是保守桶上界，不是精确 quantile；小于 1 μs 的 underflow 计入首桶，≥1 s overflow 独立计数，quantile 落入 overflow 时输出 `inf`。无样本或不适用数值输出 `-1`，不解释为零延迟。原始 TSV 同时保存完整桶计数和 worker/inline 分组。submit→body-end 早于 capture 析构和 pending 递减；shutdown fixture 的 latency 字段特指 task body wall，不与 submit latency 比较。

准确的 completion totals 在 producers 已 join、shutdown、drain 后校核 `pending=0`、`submitted=completed+discarded`、`completed=worker_completed+inline_completed`。M1 公开 API 不提供独立 join，因此最后 worker 的关闭通知可能仍在完成，`wakes` 不是严格最终通知总数；`wake_threads=NA`。M1 没有的 steal/scan 字段保持零，不能宣称这些路径被测量。`peak_threads_bound` 为结构上限，不冒充采样得到的峰值。

shutdown 的 setup、预分配和工作门等待不计入测量；warmup 至少达到指定时间。它是 benchmark-plan 规定的有限存量实验，`duration` 不是强行截断这些任务的 deadline，`window_kind=finite-inventory`；剩余程序使用固定时间窗口。存量 10^3/10^4、10/100 ms 在完整 suite 中保留，smoke 只用 32 项×0.1 ms。`task_cpu_s` 为任务 body 实际 thread CPU 累计，不能拿 sleep wall 当有效 CPU 工作。

## 资源与完整矩阵

Linux 配置同时指定 `worker-cpus` 和 `producer-cpus`（逗号分隔、不得重复），只接受当前允许集合内的 CPU。创建池前将 driver 的 affinity 临时设为 worker 集合，Linux M1 worker 按该集合轮转绑核；创建后恢复，并将 driver/producer 按 producer 集合轮转绑定。显式 affinity 失败导致运行失败，M1 best-effort warning 会拒绝正式结果。过量线程或 worker/producer 集合重叠允许但必须记录并在对照中保持一致。async/每任务线程模型在每个新线程上设置 affinity，其 OS 配置成本计入该模型；池 worker 的创建/绑核在测量窗口之前完成，这一摊销差异必须作为模型成本解释。macOS 不模拟 CPU affinity，记录 unbound，数据只供开发。

```sh
# Linux: config.json 填写实测可用集合，例如 worker-cpus/producer-cpus。
python3 scripts/run_benchmarks.py suite --config config.json \
  --repeat 5 --physical-linux-attested --output results/linux-m2-NEW
```

suite 默认覆盖 producer 1/2/4/8、四种粒度、四种树深度、三种基础模型、分配、空载及四个 shutdown 存量配置；完成 empty 配置后自动以其 median worker throughput 标定同资源空任务 latency 三组到达率。完整矩阵耗时较长。`--physical-linux-attested` 需要操作者确认固定物理 Linux，不能由 Linux uname 自动推出；没有该确认或没有明确 CPU 集合，不产生正式平台资格。macOS smoke 和本机长窗口都不能替代这个门禁。

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

可恢复源码：

```sh
git clone results/milestones/m2-baseline-NEW/source.bundle /tmp/magpie-baseline
# 如 bundle 的 HEAD 未自动检出，按 baseline.json 的 baseline_commit checkout。
```

M2 只冻结 correctness 基线与测量机制，不关闭主设计的六项性能门槛；正式报告继续按 benchmark-plan 五问解释数据和成本转移。
