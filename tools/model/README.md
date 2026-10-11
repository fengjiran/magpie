# 弱内存模型检查入口

计划工具固定为 **GenMC 0.17.0**（源码 release tag `v0.17.0`）。该版本的上游文档描述其输入为 C/C++ 程序并运行在 LLVM IR 层；本项目探针使用 C11 atomics 与 pthread API。GenMC 默认使用 RC11；探针同时检查正确的 release/acquire fence 程序和删掉 fence 后应可达的断言反例。

运行方式：

```sh
python3 tools/model/run_genmc_probes.py \
  --genmc /path/to/genmc \
  --llvm-config /path/to/llvm-config \
  --source-dir /path/to/genmc-v0.17.0
```

脚本拒绝不同版本，给每个探针设置 120 秒限制。正确 fence 用例必须退出成功、输出至少一个 complete execution 和 `*** Verification complete. No errors were detected.`，且没有 error graph。relaxed 负例必须非零退出，输出 GenMC 的 `Error: Safety violation!` 分类、marker 指定的完整 assert 表达式和对应源文件行号，并生成非空 error graph。只出现泛化 `assert`/`fail` 文本、前端函数错误或其他安全错误都不会通过。GenMC 可能在 estimation 阶段报告反例；只要其源位置、表达式和 graph 与指定断言一致，负例仍可验证工具确实到达目标状态。可用 `--genmc` 指向显式路径；不传时从 `PATH` 查找。每次实际运行都会建立一个新的证据目录，保存 `run.json`、工具版本输出、完整命令、stdout/stderr、退出码和错误 graph；`--output-dir` 可以指定一个尚不存在的目录，拒绝覆盖现有目录。源码放在 `tools/model/probes/`。

能力边界：

- 这只是工具及 atomic/fence 输入能力探针，不是 deque、MPMC 或 EventCount 的项目模型。
- RC11 结果不覆盖所有 C++20 细节、平台 ABI、操作系统 futex 行为或活性证明；各协议仍需有界模型和真实实现回归。
- 本地原生运行或 sanitizer 不是弱内存模型验证。
- 本机 capability probe 已通过。结果目录 `results/model/genmc-20261003T041133.472502Z/`（不随仓库跟踪）保存了 GenMC v0.17.0 commit、LLVM 19.1.7、完整命令、输出、RC11 探索数、断言 trace 和 DOT graph。首轮执行因 runner 对断言错误格式要求过窄而失败，原始日志保留在 `results/model/genmc-20261003T041011.380988Z/`；修订后的判据依据固定版本 trace，通过后才关闭 capability probe。

固定版本来源：[GenMC v0.17.0](https://github.com/MPI-SWS/genmc/tree/v0.17.0)、[该版本 CLI 手册](https://github.com/MPI-SWS/genmc/blob/v0.17.0/doc/manual/cli.md)。手册提供 `-rc11` 参数（并说明它是默认 memory model）和 `-dump-error-graph=<file>`；探针显式传入两者。该版本实测输出先报告 `Safety violation`，再在 trace 中列出原始 `assert(...)` 和源行；脚本据此校验完整上下文。升级工具版本时重新审查 CLI、LLVM 要求和模型边界，并更新固定版本与探针结果。

## M3 MPMC 协议投影

```sh
python3 tools/model/run_mpmc_models.py \
  --genmc build/tools/genmc-v0.17.0-build/bin/genmc \
  --output-dir results/m3/NEW_RC11_RUN
```

复用本机已验证的 GenMC0.17.0/LLVM19.1.7，不升级或安装工具。`mpmc/queue.c` 将当前 header 的 relaxed position CAS、seq acquire/release、普通 data 和 weak try 分支投影为 C11。容量2、2 producer、1/2 consumer、最多3个 accepted ticket，含物理槽复用；main join 后校核 accepted 与 consumed ID。记录 complete 和 blocked executions，blocked 不当作已验证活性。

负例只将 seq release 移到普通 payload 写之前；runner 必须看到精确 Non-atomic race 分类、marker 指定写入源行/slot data 和非空 DOT，不能把任意前端错误当反例。归档记录模型、header 和 runner hash。生产 header 的分配、type traits、指针 ownership、64位/溢出 guard 不在 C11 模型中，另用实际 C++ 正式测试；这不是完整 C++ implementation 的模型证明。

## M4 Chase-Lev 协议投影

```sh
python3 tools/model/run_deque_models.py \
  --genmc build/tools/genmc-v0.17.0-build/bin/genmc \
  --output-dir results/m4/NEW_RC11_RUN
```

固定GenMC0.17.0/LLVM19.1.7，runner拒绝版本漂移与覆盖旧目录。`deque/core.c`投影signed indices、atomic slots、普通payload发布、双侧SC fence、SC成功/失败CAS及稳定b+1恢复；容量2、owner+1/2thief、最多3个ID，含物理槽复用。普通payload只在赢得ownership后读取，main join后校核所有成功ID。SINGLE专门检查最后项竞争恢复空；BAD_RESTORE仅用改写后的expected恢复，必须到达指定一致边界断言并输出非空DOT。

`deque/owner_pops.c`固定R1交错：容量8，thief候选暂停，owner连续5次pop，然后thief认领单项；OLD_BATCH改为一次CAS领取4项，必须命中指定重复ID断言。它有意使用release/acquire门控交错，只证明该反例，不覆盖一般调度。正例要求非零complete、无error、无graph；负例检查Safety violation、marker对应源行和完整断言文本、非空DOT，不能把前端错误当反例。

这些是有限C11安全性投影，非整个C++20类型/RAII、指针对象lifetime、allocator、64位上限、操作系统或任意规模活性证明；上述边界另靠C++正式回归、sanitizer和平台测试。实际计数与source/runner/header身份见 [M4 milestone](../../docs/milestones/M4.md)。

## M5 EventCount 协议投影

```sh
python3 tools/model/run_event_count_models.py \
  --genmc build/tools/genmc-v0.17.0-build/bin/genmc \
  --output-dir results/m5/NEW_RC11_RUN
```

固定 GenMC0.17.0/LLVM19.1.7，保存模型、相关生产源和runner身份。core 用C11 SC原子及抽象的compare/park/wake覆盖1/2/3 worker、两轮注册、notify_one/notify_all并发与shutdown；并发通知组合在单worker重注册场景，多worker场景主要检查关闭握手。generic投影把CAS与predicate放在同一个模拟mutex内，futex投影把CAS和内核窗口分开。R6缺少注册后stop重查、R5旧共享generation为指定断言负例。

这不是Linux syscall运行，也不是完整C++对象或池级活性证明。模型的最终parked状态不能证明Task一定推进；M5全批迁移循环由实际池的执行项保留回归及变异负例守护。blocked单独记录，不作为活性成功。原生EAGAIN/EINTR/spurious、ABI和join后注册归零另由Linux测试验收，实际状态见 [M5 milestone](../../docs/milestones/M5.md)。
