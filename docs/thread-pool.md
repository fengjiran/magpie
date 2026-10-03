# ThreadPool

`ThreadPool` 使用固定数量的 worker 和全局有界 FIFO mutex ring。当前 M1 版本建立公共语义基线；它还没有本地 deque、MPMC queue 或 EventCount，所以不会执行 work stealing。

```cpp
#include <magpie/thread_pool.hpp>

magpie::ThreadPoolOptions options;
options.worker_count = 4;
options.global_queue_capacity = 4096;
options.rejection = magpie::RejectionPolicy::CallerRuns;

magpie::ThreadPool pool(options);
pool.submit([] { /* fire-and-forget work */ });
auto result = pool.submit_async([] { return 42; });
pool.drain();
int value = result.get();
pool.shutdown();
```

`worker_count == 0` 使用 `std::thread::hardware_concurrency()`，平台返回零时回退为 1。全局和预留本地容量都至少为 16，校验后向上取整到 2 的幂。M1 只为全局容量分配队列槽；`local_deque_capacity` 目前只校验，尚不影响调度。

`submit_async` 返回 `std::future`，不保证任务一定由 worker 执行：CallerRuns 可能在它返回前完成 future。packaged callable 的异常保存在 future 中。`submit` 任务的异常交给 `exception_handler`；没有 handler 时写入 stderr。多个 worker 和外部 CallerRuns 提交者可以并发调用 handler，因此 handler 的可变捕获状态必须由调用方保证线程安全。池不会用回调锁串行用户 handler。

外部 submit 遇到全局队列无可用槽时应用配置的拒绝策略：

- `CallerRuns` 在提交线程执行新任务，并增加 `rejected` 与 `submitted`。
- `Abort` 销毁未接受任务，并以 `QueueFull` reason 抛出 `QueueFullError`。
- `DiscardOldest` 在同一个队列 mutex 临界区移除当前可取得的全局队头并接收新任务。被移除任务在解锁并通知 worker 后销毁；若它是 packaged task，其 future 以 `future_errc::broken_promise` 就绪。`rejected` 记录溢出策略触发次数，`discarded` 记录实际移除数。

worker 在全局 ring 无槽时提交子任务会内联执行新任务，即使外部策略配置为 `Abort` 或 `DiscardOldest`。这使单队列基线的 worker 子提交保持非阻塞。Task、异常 handler 以及 task/capture 销毁始终发生在队列 mutex 解锁后。

`shutdown()` 停止接受新任务，并等待已通过 stopping 检查的提交完成发布或内联执行；它不会等待已接受的队列任务完成。`drain()` 返回于观察到 `pending == 0` 时；若仍有并发提交，返回后可再次出现任务。析构依次执行 shutdown、drain 和 worker join。

池任务、异常 handler 和 task/capture 析构不能调用本池同步 `shutdown()` 或 `drain()`，也不能阻塞等待本池 future。控制调用抛 `std::logic_error`；在本池执行上下文析构线程池会 terminate。检查覆盖 worker、CallerRuns、异常 handler、被丢弃任务和捕获对象析构。调用方必须在析构开始前阻止新的成员调用。

Linux worker 使用 `pthread_create`。非零 `worker_stack_size` 小于 `PTHREAD_STACK_MIN` 或 pthread 属性设置失败时记录 warning 并退回默认栈。`pin_to_cores=true` 时按当前允许集合中的逻辑 CPU 轮转分配；线程创建成功后再调用 `pthread_setaffinity_np`，因此 cpuset 变化或 affinity 失败只会记录 warning 并保持该 worker 不绑核，不会把 best-effort pinning 变成构造失败。M1 不读取物理核拓扑。generic 后端用 `std::thread`，会报告并忽略 stack size 和 pinning。

M1 worker 用 `std::condition_variable` 停车。stopping 谓词在与等待相同的 queue mutex 下修改；提交门和 pending 归零只在停止已发布时通知该队列条件变量。drain 使用独立 mutex/condition predicate handshake。`wakes` 统计 M1 的 condition-variable 通知调用；`wake_threads` 为空，因为该 backend 不能报告操作系统实际唤醒的 worker 数。

测试 preset 构建单独的 `magpie_test_support` 静态库来驱动确定性交错。生产 `magpie` 目标不包含测试 hook 状态或符号。

M3 可通过构建选项选择全局 MPMC 候选，默认仍 mutex。候选的 QueueFull 表示当前 slot 不可取得，可能存在 producer 未发布或 consumer 未释放窗口；DiscardOldest 不跳过未发布头，删除旧项后新项仍可能因竞争拒绝。控制/ownership/callback/future 契约继续适用，详情见 [MPMC 使用说明](mpmc-queue.md)。
