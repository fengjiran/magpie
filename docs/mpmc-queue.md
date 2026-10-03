# M3 全局 MPMC 候选

默认 ThreadPool 继续使用 M1 mutex ring。`MAGPIE_QUEUE_BACKEND=mpmc` 编译选择 M3 全局 MPMC + condition_variable 候选，公共 submit/future/控制 API 不变，没有本地 deque、work stealing 或 EventCount。选择不会在运行时切换，`global_queue_backend()` 返回当前库的构建选项，原 `BuildInfo` 布局保持不变。

```sh
cmake --preset release-mpmc
cmake --build --preset release-mpmc --parallel 2
python3 scripts/run_ctest.py --build-dir build/release-mpmc --mode fast
python3 scripts/run_ctest.py --build-dir build/release-mpmc --mode stress
```

对应 `tsan-mpmc`、`asan-ubsan-mpmc` 和 production-only `bench-mpmc` preset 已提供。静态/共享外部 consumer 传递相同 backend 选项；生产目标不编译 test hooks。Linux job 已配置两个 backend 的 GCC 三变体及 Clang Release，不代表远端已经执行。

## 原语与所有权

`<magpie/mpmc_queue.hpp>` 的 `MPMCQueue<T>` 存 T，要求 trivially copyable、默认/拷贝构造及赋值不抛；池实例化 `MPMCQueue<Task*>`。`enqueue(T)`、`dequeue(T&)` 和 `dequeue_bulk(T*,limit)` 使用值类型，bulk 每项独立 CAS。构造接受已规范化的 2 的幂容量≥2，池层容量仍≥16并向上取整；按实际 Slot byte size 校验。初版要求64位、原生 lock-free size_t atomic，指针载荷不得为空。

position CAS 只认领 ticket；seq acquire/release 传递普通 data 的访问权。只有成功认领对应位置的线程读写槽载荷，claim 之后生产路径没有分配、回调或可抛操作。配置错误抛异常，索引上限在 Release fail-fast；物理槽反复复用，整数索引不回绕。head/tail 按 `MAGPIE_CACHE_LINE` 配置对齐，不宣称已测得硬件 cache line。

try 返回 false 表示当前 unavailable，输出参数保持原值、未转移所有权。生产者认领头部但未发布时，后继可以已发布，dequeue 仍失败；消费者认领但未释放时，即使逻辑项数小于 capacity，enqueue 仍可失败。不能由此断言严格空/满，也不承诺严格 FIFO try 的线性化或 lock-free progress。消费位置顺序与执行/完成顺序分开。

成功发布后消费者可以立即删除 Task，pool 只覆盖托管字段为空，不读取已发布指针。失败 enqueue 保留原 Task 托管，按 CallerRuns/Abort/DiscardOldest 处理；完成/丢弃/拒绝回退都先销毁 Task、再统一 dec_pending。worker 内 unavailable 提交仍强制 CallerRuns。

DiscardOldest 是一次当前头部 dequeue，不跳过未发布头。每次删除旧项后，解锁/离开原语再销毁旧 Task，其 future broken_promise 就绪，再递减旧 pending，重试新项；其他 producer 或 capture 析构重入可抢占刚释放的槽。当前主设计 `DISCARD_RETRY_LIMIT=2`：取头失败或两次删除后新项仍不可入则抛 QueueFull；已经丢弃的旧项不会复活，incoming 任务销毁并回退计数。每次 incoming 提交只计一次 rejected，实际删除数计 discarded。

## condition_variable 握手

worker 先在停车 mutex 外 try_dequeue，失败后持原 queue/parking mutex 再探测，之后判断 stopping→gate→pending，再 CV wait。Task 在 queue 调用完全返回、停车 mutex 释放后执行。

每次成功发布先完成 seq release，再获得这把停车 mutex 并 notify_one。若发布发生在 worker 最后探测和实际 wait 之间，notifier 必须等 worker 原子释放锁并等待，通知不会丢失。head reservation 恢复发布也走同一通知路径。停止发布及关闭中的 gate/pending 归零继续持该 mutex notify_all；构造回滚也沿用此协议。

这个 adapter 没有 EventCount、epoch 或 WaitSlot。MPMC 原语无 mutex，池成功提交仍有通知 mutex，以及 gate/pending/submitted、Task allocation 等共享成本；不能声称整个池是 lock-free 或零共享。

## 验证边界

正式原语测试覆盖 FIFO（仅单线程）、物理复用、两种 reservation hole、ID 与每个成功 position→ID 的历史配对、bulk、配置和 Release 索引上限。百万任务原语/池 stress 逐 ID 校核，weak try 失败不套严格 FIFO 空/满 oracle。

真实池确定性交错覆盖：后继发布后 worker 再次停车、首项恢复发布；未发布头下 DiscardOldest；consumer 已认领未释放槽；capture 析构重填释放槽导致两次 discard 后 incoming 拒绝；所有其他任务退休后拒绝任务的 pending 1→0 唤醒已经等待的 drainer。原有 M1 控制、异常、future、关闭和构造回滚 suite 继续运行。

新活性用例使用10s/120s watchdog、CTest独立子进程和二级 timeout；超时输出 atomic ticket/seq 或独立保存的 pool snapshot 后 `_Exit(124)`，自测确认不会卡在 join 析构。pool snapshot 是最后 hook 点逐原子读取的近似记录；portable watchdog 不提供所有线程堆栈，原有 M1 完整失败现场缺口仍未完全关闭。

GenMC 0.17.0/LLVM19.1.7/RC11 检查 capacity2、2 producers、1/2 consumers、3 tickets 的 C11 协议投影，包含 slot reuse 和 weak unavailable；正例及故意提前发布 token 的 non-atomic race 负例分别校核。它覆盖核心 atomic/data 协议，不能替代 C++ allocation/traits/ABI/指数上限测试，也不是活性或任意规模证明。模型入口见 [tools/model](../tools/model/README.md)。

实际命令、源码身份、性能五问与未关闭门禁见 [M3 milestone](milestones/M3.md)。只有正式平台/性能门禁关闭后才评估默认切换；本机 macOS 数据供开发，不作为物理 Linux 验收。
