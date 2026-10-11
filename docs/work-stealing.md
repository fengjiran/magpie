# M4 工作窃取候选

默认构建继续使用 mutex 全局队列。M4 是显式 opt-in 的 MPMC + bounded Chase-Lev + condition_variable 候选；M5 可再选择 [EventCount、退避与 futex/generic](event-count.md)。

```sh
cmake --preset release-workstealing
cmake --build --preset release-workstealing --parallel 2
python3 scripts/run_ctest.py --build-dir build/release-workstealing --mode fast
python3 scripts/run_ctest.py --build-dir build/release-workstealing --mode stress
```

也可设置 `MAGPIE_QUEUE_BACKEND=mpmc` 和 `MAGPIE_ENABLE_WORK_STEALING=ON`；mutex + work stealing 的组合在配置阶段报错。`tsan-workstealing`、`asan-ubsan-workstealing`、`bench-workstealing` 提供独立目录。`scheduling_backend()` 返回 `global` 或 `work-stealing`，不新增运行时策略或修改 `BuildInfo` 布局。

## 所有权与容量

- 本池 owner worker 提交到自己的 deque；TLS 同时核对池实例。外部线程和跨池提交走全局队列，外部 CallerRuns 不获得 owner 权限。
- `local_deque_capacity` 在该候选中实际分配每 worker 的原子指针槽。池容量至少16、向上取整至2的幂；默认1024。原语允许至少2的幂容量，用小容量做模型与竞争回归。
- 本地满只转投当前新 Task* 到全局一次；全局当前不可用则 worker CallerRuns，即使外部策略是 Abort/DiscardOldest。不搬迁旧半批、不设 overflow 缓冲、不丢弃本地旧项。
- 单元素 steal 的每个成功项各有一次 top CAS。失败候选可能已被赢家销毁，失败者不读取其对象、不执行或销毁它。
- owner 按 LIFO pop，thief 领取当前 top；不承诺全池 FIFO、公平性或饥饿上界。本池任务仍不得阻塞等待本池 future；没有 helping wait。

## 获取与停车

worker 先 local pop，再全局 bulk（至多 `min(32, local_capacity)`），再随机选择一个 victim 逐项 steal（至多 `min(64, local_capacity)`）。接收只发生在 owner pop 返回空后；每个非空批保留第一项作为当前执行项，其余项逆序入本地，完成发布后才执行保留项。保留项不再暴露给 thief，避免只有迁移而没有执行；其他项的开始/完成顺序仍受竞争影响。

常态探测不持停车 mutex。准备等待时取得 CV mutex，重新探测本地/全局，并按预分配随机 permutation 完整探测所有其他 workers，再判断退出或 wait。全部 WorkerCtx、deque、victim 列表和批 buffer 在任何线程启动前构造，地址稳定；构造失败沿用 stopping→通知→join→重抛。

CV 模式每次成功本地发布与批剩余项发布后补通知，通知持同一个停车 mutex，与最后探测→wait 配对。只有一个保留执行项时没有再次发布。关闭继续按 stopping→提交门→pending 裁决；不把 approximate depth 或正在执行作为免通知依据。M5 模式在注销当前 WaitSlot 后补通知，不能把 CV 验证称为 futex 验证。

## 成本与验证边界

保留 release 发布、pop/steal 两侧 SC fence 和成功/失败均 SC 的 CAS；有符号64位索引不回绕，索引上限与非法配置分别 fail-fast/抛异常。`0≤bottom−top≤capacity` 只在受控稳定边界核对，任意两次独立 load 不是一致快照。

本地路径仍分配 Task 包装，更新 gate/pending/submitted，并取通知 mutex；批领取/迁移可能在队列间暂存已认领指针。新增每 worker 本地槽、64项批 buffer 和 `worker_count−1` 个 victim 索引，victim 列表总空间为 O(worker_count²)。这不是全池 lock-free 或零共享/零分配的承诺。

`stolen` 统计成功单元素窃取次数，同一任务再次迁移可以多次计入；`local_spills` 统计本地 try_push 失败后的转投尝试，不是丢弃数。原有 completed/worker_completed/inline_completed、pending、异常、future 和生命周期 guard 语义继续适用。

实际本机测试、RC11投影、开发性能数据与剩余 Linux/ARM64 门禁见 [M4 milestone](milestones/M4.md)。模型不是整个 C++ 实现或任意规模活性证明，默认切换仍需平台和性能证据。
