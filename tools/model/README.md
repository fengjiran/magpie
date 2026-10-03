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
