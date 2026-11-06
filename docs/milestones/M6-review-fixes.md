# M6 审核修复与证据（2026-11-05）

- 当前状态：correctness 修复、测量修复、关停资源优化和归因检查已实施；**M6 仍进行中，gate 2/4/7 不关闭**。低载精确样本 p99 比为 1.060，空任务 T8/T1 为 0.485。
- 日期按 Asia/Shanghai；本机原始目录名采用 UTC：`results/repair-20261104T155027Z/`，不随仓库跟踪。
- 起始 revision：`fbe112aae59918b0adfe95d1b63fe016acfc6851`。起始 dirty 文件为主设计、M6、thread_pool.cpp，另有未跟踪 `magpie_bench`。`initial.json`/`initial.diff` 保留原始状态；未提交、未推送，原有批接收容量证明注释及历史归档保留。
- 平台：Linux x86_64 / WSL2，内核 `6.18.54.1-microsoft-standard-WSL2`。Release/ASan 使用 GCC 14，TSan 使用 Clang 18；正式对照固定 worker CPU0–7 的相应子集、driver/producer CPU8。

## 修复内容与协议

**CV 最后检查。** 恢复锁外常态探测、锁内 local/global/全 victim 检查及 wait 循环。撤回锁外全扫描加锁内 local/global 轻查：victim 可在被扫描后完成发布和通知，使之后的等待丢醒。`LocalPublishBetweenProbeAndPark` 在锁外探测返回后暂停，等待另一个 worker 完成 child 发布和通知，再放行；parent 保持占用，由空闲 worker 执行 child。成功不依赖 sleep，独立 watchdog 兜底。

**关停期停车。** EventCount 在 stopping 后仍有 pending/gate 时允许重新等待；注册后检查完整 `stopping → gate → pending` 退出谓词，再做最后任务扫描和 epoch 复查。停止后的 pending 1→0、gate 1→0 分别广播 worker。通知在注销/释放 drain 锁后进行；Task 执行、回调、销毁顺序和 deque/MPMC 内存序保持原协议。

**最终 pending 的可见性配对。** 初稿只增加了条件通知，原生测试通过，但 RC11 找到真实反例：完成线程写 pending=0 后读 stopping=false 并省通知；另一线程随后停止并通知；晚注册者能读到旧 pending=1 后入睡。SC stopping 的总序不自动传递另一个 acq_rel 原子的写入。

修复将最终 pending 的 stopping 检查放入原有 drain mutex，并在同一 mutex 下发布 stopping。完成方先解锁时，其零值经 mutex→停止通知对晚注册者可见；停止方先解锁时，完成方必见停止并补通知。不对每次完成新增锁或共享 RMW，也不放宽原子内存序。CV 的锁序为停止发布时 queue→drain；完成方先释放 drain 再取 queue，不产生反向持锁环。generic 每槽通知也在 drain 解锁后执行。

**控制线程等待提交门。** 新 CallerRuns 负载显示原 yield 等待约 100ms 时消耗约 102ms CPU，改为 C++20 atomic wait，最后离门在停止后 notify_all 控制等待者，再通知 worker。`GateZeroBeforeControllerWait` 强制归零通知先完成，再允许控制线程 wait。仍按成功观察 gate=0 返回，不承诺持续拒绝流下有界返回，不改变外部对象 lifetime 契约。CMake 新增 atomic wait/notify 编译链接探针，采用与库相同的 Threads 依赖；公共 API/Stats 未增加字段。

**布局。** 给 drain 同步区及 handler 起点添加配置缓存行对齐，防止后继对象进入 pending/inline_completed 所在行。Clang record layout（配置 64B）验证：pending=960、drain_mutex=1024、inline_completed=1216、handler=1280；Impl sizeof 从 1216 到 1344。此为配置边界，不声称测量了硬件缓存行。

**精确样本与归档。** 保留 `*_us_upper`/原 histogram，新增 nearest-rank 样本 p50/p99/p999，按 total/worker/inline/arrival lag 分组。排序、临时存储和归档都在窗口结束后，任务热路径不新增打点。runner 的 latency 进程另存纳秒样本和 SHA-256；warmup 不写归档，已有文件拒绝覆盖。旧归档缺精确列时 `compare_p99` 返回 insufficient-evidence，不把桶上界重命名使用；样本分位数精确不等于估计没有误差。

关闭 harness 新增 single-tail、mixed、caller-runs；后者用逐一启动的阻塞 root 占满 worker，再填全局队列，由额外 producer 内联执行最后任务。其 task CPU 包含 shutdown 窗口，不能拿 drain_cpu 减全部 task_cpu 当纯忙等。

## 实际正确性验证

每个配置执行 `cmake --build build/<配置> --parallel 2`，随后：

```sh
python3 scripts/run_ctest.py --build-dir build/<配置> --mode fast
python3 scripts/run_ctest.py --build-dir build/<配置> --mode stress
```

| 配置 | fast suites | stress suites |
|---|---:|---:|
| release（mutex/global/CV，含 benchmark 测试） | 22/22 | 3/3 |
| release-mpmc | 18/18 | 3/3 |
| release-workstealing | 18/18 | 3/3 |
| release-eventcount（futex，含 benchmark 测试） | 23/23 | 3/3 |
| release-eventcount-generic | 19/19 | 3/3 |
| tsan-workstealing | 14/14 | 3/3 |
| tsan-eventcount（含 benchmark 测试） | 19/19 | 3/3 |
| asan-ubsan-workstealing | 18/18 | 3/3 |
| asan-ubsan-eventcount（含 benchmark 测试） | 23/23 | 3/3 |

数字为 CTest suite/进程数量，包含不适用 backend 的预期 skip，不是全部 GTest case 数。原有 R1–R7、百万唯一 ID、异常/future/retirement、部分构造失败、真实 Linux EAGAIN/EINTR/spurious 和静态/共享消费者继续执行。ASan 在沙箱外运行，保留全部内存/泄漏检查；未禁用 LSan、断言或诊断。

新增的最终 pending、syscall 前最终通知、pending 始终为零的最后 gate 回归，均在任何额外 shutdown/析构之前等待 worker 退出，避免收尾广播掩盖缺陷。临时变异分别删除 CV 最后 victim 检查、最终 pending 广播、最终 gate worker 广播，三者均被对应 watchdog 以 124 拒绝，日志为 `final-mutant-{cv,pending,gate}.log`。变异不进入生产源码。

分位数测试以全部 50us/60us 样本验证：旧桶上界相同，新 p99 能识别 20% 回退；另测 nearest ranks、执行分组、原始样本一致性和拒绝覆盖。GCC/Clang 各有 fresh BUILD_TESTING=OFF 生产配置/构建，验证新的标准库 facility 门禁。文档骨架检查使用 `CXX=clang++-18 python3 design/validation/check_design.py`，仅作为辅助证据。

原始命令、退出码及全部输出在 `*-acceptance.log`、`fresh-production-probes.log`、`design-fragments.log`。

## 固定工具与模型

本机原固定工具缺失。在仓库 `build/tools` 隔离解包 LLVM 官方 19.1.7 包，校验索引中 SHA-256；按 [GenMC v0.17.0](https://github.com/MPI-SWS/genmc/tree/v0.17.0) 固定 commit `29b03a66402c4453fc77901ef3be90bb55707cd4` 构建。没有系统安装、升级或全局环境修改，包版本/哈希见 `llvm19-packages.json`，完整构建见 `genmc-build.log`。运行环境仅对工具子进程设置隔离 LLVM 的 LD_LIBRARY_PATH。

先重跑能力正反探针，再执行：

```sh
python3 tools/model/run_event_count_models.py \
  --genmc build/tools/genmc-v0.17.0-build/bin/genmc \
  --output-dir results/repair-20261104T155027Z/event-models-rc11-final
```

实际命令显式使用 RC11，并关闭自动 memory-model detector，避免全 SC 输入被自动改为 SC。**15 个有界正例、7 个指定负例的判据通过**；正例 complete 非零且无错误图，负例验证指定源行、完整断言及非空反例图。最终 run.json 中所有被记录的模型/协议源 hash 与交付源码一致。

范围：原 core 覆盖独立等待字、注册/注销/重注册和停止；closing 单 worker 枚举停止与完成竞态，双 worker 枚举已停止后的最后 pending/gate 广播，两轮注册也在已停止条件枚举；gate_wait 抽象标准 atomic wait 的比较/停车语义和两个控制者，不模拟某个 stdlib 内核实现或无限拒绝流。

保留的失败：`event-models/` 是初稿未配对 stopping/pending 的真实反例；`event-models-v2/` 的双 worker 完整停止竞态超过 120s，分解后的范围不能冒充该范围穷举通过；`event-models-final/` 的控制模型漏掉实际存在的停止 mutex 串行化，出现 unordered-write warning/error graph 后被 runner 拒绝，随后补齐并用严格 RC11 重跑。模型不是整个 C++、任意规模活性、Linux syscall 或 ARM64 证明。

## 独立机制对照

冻结 snapshot+binary+SHA-256，统一 GCC `-O2 -DNDEBUG` 和同一新 harness。没有创建项目 commit；HEAD、dirty diff 和逐文件 hash 共同标识源码。目录分别为 before-shutdown/eventcount、after-parking-v2、after-layout、after-gate-wait、mutex-final。原初未配对的 after-parking snapshot 保留为失败候选，没有作为最终停车对照。

`run_repair_bench.py` 串行执行主矩阵，每点 5 独立进程，A/B 每轮交错顺序；每进程 warmup≥2s、固定窗口 10s，关闭实验按有限存量结束。构建/测试不与正式采集并行。15 组主矩阵共 120 进程，布局复测另 10 进程；全部原始数据、环境、顺序、命令、源码/binary 校验与可复现脚本在归档中。采集成功不代表性能门槛通过。

| 独立变更/负载 | before 中位数 | after 中位数 | 解释 |
|---|---:|---:|---|
| 关停停车，W4/单个 100ms 长尾，process CPU | 0.299567s | 0.000254s | 相同任务/资源，墙钟约 100ms |
| 关停停车，mixed 存量，process CPU | 0.002131s | 0.000675s | 原均匀存量之外的尾部成本 |
| 关停停车，uniform 存量，process CPU | 0.003720s | 0.000962s | 仍包含调度/时钟成本 |
| 控制 atomic wait，CallerRuns 100ms，shutdown CPU | 0.102070s | 0.000056s | worker 停车和布局已固定，仅改变控制等待 |
| 布局，频繁 drain 吞吐 after/before | 首轮 0.947 | 复测 1.047 | 两轮方向相反，未证实稳定收益或回退 |

布局保留用于落实隔离约定，不宣称因此加速。CPU 差值不是纯忙等的独立剖析值；CallerRuns 的整个任务 CPU 和 shutdown/drain 窗口分开解释。

## 精确 p99 和扩展性门槛

先测两个新 binary 的 W1/P1 空任务容量，取较低中位数 4.067M/s，按其 10/50/90% 产生**相同绝对到达率**供两个版本对照；采样 stride=256、capacity=262144，未耗尽样本。比值为各进程样本 p99 中位数之比。

| 到达率比例 | mutex p99 | candidate p99 | 比值 |
|---|---:|---:|---:|
| 10% | 57.928us | 61.403us | **1.060，超过 1.05** |
| 50% | 71.191us | 68.502us | 0.962 |
| 90% | 235.898us | 102.004us | 0.432 |

低载两侧均无拒绝；其 gate 4 数值判定本轮未通过，不改阈值。90% 点 mutex 少发到达项中位数约 3.29M、candidate 为 195，且两侧都有拒绝，不能用该点较低 p99 宣称同等实际服务负载下全面胜出。原始到达 lag、refused、missed_arrivals 与纳秒样本一并保存。无新峰和统计置信性仍需单独评估。

| payload | T8/T1（worker leaf） | 每叶 CPU 放大 |
|---|---:|---:|
| empty | 0.485 | 13.788 |
| cpu/work=64 | 1.200 | 5.635 |
| cpu/work=256 | 3.093 | 2.270 |

固定一个 driver，inline=0。CPU 放大使用 process CPU/leaf，包含 driver/root，不能称纯 worker kernel CPU。gate 2/7 保持开放，未以提高 producer 数或内联工作制造 scaling。

## 归因与边界

硬件 cycles 事件不可用；软件 `cpu-clock:u` 可用，随后对**未插桩的冻结最终 binary** 做独立 99Hz 采样。记录 6725 样本、lost=0。当前 perf 缺完整符号/展开依赖，未安装新系统工具；按同一 ELF 的 nm/objdump 解析平面 IP，不宣称调用栈剖析。结果在 `software-profile/`，不混入正式吞吐 A/B。

| 平面样本邻域 | 代表 IP | 样本比例（局部汇总） |
|---|---|---:|
| pending reserve/retire 的共享访问及 RMW 邻域 | e6be/e6d8/d4e9 | 约 30.0% |
| admission gate 增减邻域 | e692/e6a6/e832 | 约 23.0% |
| submitted 增量邻域 | e816 | 4.24% |
| epoch CAS 邻域 | fb49/fb5c | 7.51% |
| pause 循环邻域 | e3d2 | 15.12% |

这些是用户态软件采样的指令区域，可能存在 skid，不能当各条 RMW 的精确独立 CPU 成本。反汇编位移与 Impl 布局一致：gate 0x380、pending 0x3c0、submitted 0x480。热点证据支持优先研究共享元数据和运行期退避，不支持仅凭 stolen/epoch 聚合数锁定迁移通知为主因。

另建仅在归档中的 notification-diagnostics 变体：owner 独写分项计数，每 1024 次调用随机选一个位置测 wall/thread CPU；Task 自有 steal 计数在 ownership 转移后更新，join 后汇总，没有新增全局每任务诊断 RMW。诊断改变 Task 布局和调度，**不作生产吞吐结论或严格 CPU 份额分解**，计时包含采样时钟开销。

该变体 W8/empty 中，migration 通知占 owner notify_one 调用约 0.98%、扫描约 5.16%；至少被窃取两次的已完成任务占约 3.46%（含 root）。histogram 总数与 worker_completed、加权次数与 stolen 配平。存在重复迁移，但当前证据不支持“迁移通知是 CPU 放大主因”的旧表述。故本轮不修改迁移算法、不省发布通知、不放宽 fence，也不引入 allocator/分片/NUMA。

完整数据汇总在 `analysis.json`，原始诊断源、构建命令、binary hash、输出及采样限制独立归档。

## 交付与剩余范围

主设计、ADR-002、测试/benchmark 计划、用户说明和 M6 当前状态已同步。GCC `fclose` 函数指针属性警告改为 lambda deleter，没有屏蔽 Werror；最初 CV 测试误用未链接的 build-info 符号、冻结 snapshot 漏 spin_wait.hpp 等首轮失败日志均保留。文档辅助检查默认 clang++ 不存在，使用已安装 clang++-18 后通过。

本轮验证集中于 Linux x86_64/WSL2 的 futex/generic。新协议的 macOS、ARM64、Windows、远端 CI、小时级 soak 及完整 M6 全部历史矩阵未执行。默认仍 mutex；M6 的 gate 2/4/7 不关闭，M7 不推进。布局收益未证实；运行期 CPU 放大与低载 p99 仍是后续优化目标。

## 2026-11-06：复审后的文档与判定链修复

起始 HEAD 为 `3019a3f`，工作区 clean。本轮未修改线程池运行时、异常投递策略或性能阈值，也未提交。

- 撤销“异步异常同时进入 future 和 handler”的错误说明。packaged callable 的用户异常只入 future；裸任务异常交给 handler；handler 再抛仅记录并继续。同步纠正审核报告的 L-2 判断、处置记录和异常分支说明，既有 `ThreadPool.FutureException` 继续守护。
- `check_design.py` 入口传播 main 返回值；缺失或空 CXX 均返回进程退出码 2。新增独立 fast Python 用例，不依赖 benchmark 开关；正常 CXX=clang++-18 编译与辅助回归也通过。
- replay 在执行测量前核验全部已登记 `samples_sha256` 的文件，缺失/损坏拒绝且不创建新输出。回归覆盖五份样本中的后续文件损坏，并验证完整样本可继续 replay；原先无样本哈希的聚合归档兼容路径继续通过。
- p99 比较要求正整数样本数，拒绝 NaN/inf、小数、布尔、零/负数和不可解析值；无效分位数同样返回 insufficient-evidence。

先加入回归并执行，旧实现的 CLI 检查和 runner 测试均失败，原始输出分别保留为 `checker-before.log` 和 `runner-before.log`；实现修复后 CLI 测试、10 个 runner 用例（含子用例）、正常设计辅助检查均通过。最后执行：

```sh
cmake --build build/release --parallel 2
python3 scripts/run_ctest.py --build-dir build/release --mode fast
```

Release fast 为 **23/23 suites**，包含新 `magpie.design-validation-cli`、runner、原始测量集成和既有异常语义回归。原始日志与初始状态存于本机 `results/repair-validation-20261106T145849Z/`；最终 fast 日志为 `release-fast-final.log`。归档判定夹具只验证检查逻辑，不是性能或物理平台证据。

本轮未重跑 stress、sanitizer、模型或性能矩阵；运行时/原语没有变化，前文与复审的相关证据仍按其日期和范围引用。M6 gate 2/4/7 状态保持不变。
