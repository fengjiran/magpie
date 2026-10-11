# M1–M3 审核问题修复（2026-10-04）

- 基础 revision：`c6411cd0f4f94d37c3218f943a111c9e5299017c`；开始时已有未提交 M4 改动，本轮在该工作区作局部修复，没有覆盖 M4 文件或修改 HEAD/index。
- 范围：两个 `submit_async` ownership/type-contract 问题及 benchmark smoke 资格问题。公共 API、队列、提交门、pending 和停车协议不变。
- 状态：三个问题的实现与本机回归通过；修复后的 M2 开发冻结、同日原 binary replay 和归档恢复校验已完成。Linux 原生、ARM64 和正式性能门禁仍未关闭。
- 原始命令、日志及源码 hash：本机 `results/reviews/m0-m3-fixes-20261004/`，不随仓库跟踪。原审核证据保留在 `results/reviews/m0-m3-20261004/`。

## 修复

1. `detail::AsyncTask<F>` 直接拥有原始 F，禁止复制/移动以固定地址。packaged_task 只保存借用 Task 的轻量 callback；future 可保留该 callback 的状态，但不拥有 F。Task 在既有 ExecutionScope 中删除时销毁 F，先于 pending 递减。正常结果、callable 异常和未执行任务的 broken_promise 继续由 packaged_task 提供。
2. 从提交参数直接构造 F 一次，不额外移动用户 callable；支持 CopyOnly lvalue，throwing move 构造失败正常传播。Task 用 unique_ptr 托管至非模板入口接管；不新增 future 投递协议，也不收紧 TaskCallable。
3. `formal_platform_eligible` 明确排除 smoke。replay 继续使用冻结配置，显式 smoke 即使满足长窗口也不能获正式资格；旧归档中的 smoke=true 同样排除于正式 A/B，不信任旧 runner 写入的 eligible=true。

这是头模板修复，已有消费者需要重新编译才能使用修复后的 submit_async；仅更换预编译库不能更新旧调用点中的模板代码。返回的引用仍由调用方保证被引用对象 lifetime，不能引用 Task 自有 capture。

## 本轮验证

平台：Darwin x86_64、AppleClang 17.0.0、generic std::thread + CV。Release 自有目标保留有效 `-O2` 和 warnings-as-errors。下面的数量为 CTest suite 数量；不适用 backend 的候选交错会按既有逻辑 skip。

| 配置 | fast | stress |
|---|---:|---:|
| Release mutex/global | 17/17 | 3/3 |
| Release MPMC/global | 17/17 | 3/3 |
| TSan MPMC/global | 14/14（不构建 death suites） | 3/3 |
| ASan+UBSan MPMC/global | 17/17 | 3/3 |
| Release MPMC/work-stealing（当前 M4 集成回归） | 17/17 | 3/3 |

配置命令使用独立 `build/review-fixes-{mutex,mpmc,tsan,asan-ubsan,workstealing}`，显式设置 queue backend、work-stealing、sanitizer，启用 benchmark 与独立 test-support。每个目录执行 configure、`cmake --build ... --parallel 2`、`scripts/run_ctest.py --mode fast` 和 `--mode stress`；逐条命令和退出码见上述归档的 `*/{configure,build,fast,stress}.command.json`。fast 包含独立 static/shared consumer。

新增不依赖 GTest 的 `magpie.async-contract`，有 10s watchdog 和 CTest 20s 外层超时。覆盖保留 shared_future 至 pool 析构以后、正常/异常 callable retirement、DiscardOldest broken_promise、Abort/CallerRuns capture guard、销毁先于 pending 退休、CopyOnly lvalue、二次 move 会抛的 callable、首次构造抛异常的计数、void/reference/move-only result。

同一 async-contract 源码另外以 AppleClang + 本机已有 libstdc++ 11.2 编译真实 mutex/MPMC 池，两组通过，构建和执行记录在 `libstdcxx/`。只为这两次进程设置 DYLD_LIBRARY_PATH 解析已安装 dylib；没有安装/升级工具。这证明已复现的标准库 ownership 差异被覆盖，不是 Linux pthread 或 GCC Linux 验收。

runner 5 项 unittest 通过；新增 replay 用例保留长窗口配置，同时验证正式 control、当前 smoke 和旧 smoke 归档。机器与测量 subprocess 使用 mock，只作为资格判定逻辑回归。主设计骨架 `python3 design/validation/check_design.py` 通过，不能代替真实库测试。本轮未改原语内存协议，没有重复 GenMC 模型或宣称新的弱内存证明。

## M2 冻结与剩余门禁

旧 `a0cb34e3a5ee180a97df8bdba22e928180f84c92` 冻结源码包含已确认的 async 缺陷；保留 binary/bundle/raw 数据用于历史追溯，不再作为修复后 correctness 基线。

新开发冻结使用默认 mutex/global，独立源码 commit 为 `0543294087506d5f25d60a8fdb2d7f2b85ca203a`；binary SHA-256 为 `457f1c769a8a5b195136e7dc84f9a54ae2480e323c0cfb19b517ea79c295e680`。freeze 在临时 clone 生成 commit，不改变主工作区 HEAD/index。该快照包含本次修复及已有 M4 源码，但工作窃取编译选项为 OFF；本文冻结完成后的证据补记不影响已冻结 runtime 源码。

```sh
python3 scripts/run_benchmarks.py freeze --config bench/configs/empty.json --repeat 5 \
  --output results/milestones/m2-frozen-mutex-review-fixes-20261004
python3 scripts/run_benchmarks.py replay \
  --baseline results/milestones/m2-frozen-mutex-review-fixes-20261004 --repeat 5 \
  --output results/milestones/m2-frozen-mutex-review-fixes-replay-20261004
```

快照 fresh Release fast 17/17、stress 3/3；生产最终 `-O2`、无 test hooks、binary/bundle/initial raw.tsv 校验和及 bundle 恢复的全部源码 hash 均通过。初次与 replay 各 5 个独立进程，每次 warmup 2s、测量 10s；同日、记录的环境和 config 相同。replay/initial median ratio 为 1.01535，`formal_ab_eligible=false`。原始 median/IQR 在各组 `summary.json`，重跑 IQR 约为其 median 的 22.6%，不据此声称低噪声稳定性或性能提升。

正式基线仍需固定物理 Linux 完整矩阵；本机修复不关闭 M0/M1 原生平台或 M3 正式性能门禁。新基线只提供修复后开发冻结与重跑证据，不改写旧 M3/M4 性能对照结论。
