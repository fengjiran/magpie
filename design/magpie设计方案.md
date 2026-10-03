# magpie 设计方案

> 修订：2026-10-03，审核修复版本。状态：**设计已修订；M1 mutex 正确性底座已在 macOS 本机实现和验证，Linux worker backend 尚待 CI/原生验证**。
> [审核报告](review-2026-10-03.md) 保留旧版反例；[修复对照](review-fixes-2026-10-03.md) 记录逐项处置。旧版批量 CAS、overflow 与共享 futex 字协议全部撤回。

## 1. 文档目的与约束

本文件定义项目定位、组件职责、公开 API、关键原语骨架、所有权、关闭协议和验收。三个 ADR 补充协议依据；test-plan/benchmark-plan 分别定义正确性与性能证据。主设计与 ADR 冲突时先修正文档，不靠实现自行选择口径。

当前实现是固定 worker + 全局有界 mutex ring 的 M1 基线，使用 condition_variable 停车。M3 已新增可选全局 MPMC + condition_variable 候选（默认仍 mutex），还没有 Chase-Lev 或 EventCount；下文涉及这些原语的代码段仍是设计骨架。GenMC atomic/fence capability probe 不能替代项目原语模型、Linux pthread 原生验证、Linux futex、ARM64 或性能验收。

M2 已新增 opt-in benchmark harness、独立源码快照与基线重跑入口，未改变上述生产协议。
测量边界与不适用维度见 [benchmark 使用说明](../docs/benchmarks.md)，实际验收状态见 [M2 milestone](../docs/milestones/M2.md)。

核心 invariant：

1. 一个接受任务恰有一个执行者或显式丢弃者；队列间只传 Task*。
2. pending 在可执行发布前增加，在执行/销毁完成后递减。
3. dequeue/steal 的竞争失败不产生任务所有权。
4. 用户代码只在队列操作完整返回后运行；注册等待期间只探测与认领，不运行用户代码。
5. stopping、提交门、pending 联合裁决关闭；登记等待后再次检查 stopping。
6. 池对象存活期间才可调用成员；提交门不能代替对象外部的 lifetime 管理。

## 2. 项目定位与设计原则

magpie 是固定 worker 数的通用 C++20 工作窃取线程池，主要服务 CPU 短任务、嵌套非阻塞提交、突发批处理。混合/阻塞任务可运行但会占用 worker；不承诺阻塞任务下保持 CPU 并行度。

初版：全局有界 MPMC + 每 worker 有界 Chase-Lev + 池级 EventCount（每 worker 停车槽）+ spin/yield/阻塞退避。

正确性先于性能；高性能是需要 benchmark 验证的目标。任务执行仍更新共享 pending，提交仍更新 gate/pending/submitted/epoch；本架构减少队列争用，**不是 share-nothing 或每 task cycle 零共享**。CallerRuns 在提交线程执行，实际同时执行的任务数可超过 worker_count。

## 3. 业界参考与 trade-off

| 参考 | 借鉴 | 本项目边界 |
|---|---|---|
| Chase-Lev / Taskflow | owner LIFO、thief 单项 FIFO、完整配对屏障 | 不扩展一次 CAS 认领半批 |
| ForkJoin / oneTBB | 工作窃取与 helping wait | 初版没有 helping wait，任务内不得阻塞等待本池 future |
| Vyukov MPMC | 有界、每槽序号发布/释放 | try 失败表示当前不可取得，不等于严格空/满 |
| EventCount / futex | 注册、最后探测、比较等待 | 每 worker 独立状态字，避免旧 key 回写共享字 |

资料以官方源码、论文及平台手册为准，具体协议见 ADR。线程数取逻辑核并非对所有阻塞负载都最优；spin/yield 参数、绑核及缓存布局均由实际数据决定。

## 4. 总体架构与职责

- 外部 submit：全局队列；不可入队时按配置的拒绝策略。
- 本池 owner worker submit：本地 try_push；满时把**当前新任务**转投全局；仍失败则 CallerRuns。已有本地任务不移动。
- 本地 deque：只负责 try_push/pop/单元素 steal，没有全局队列/EventCount/统计引用。
- 池层调度：批量接收、重复单项窃取、通知、策略、执行和计数。
- EventCount：一个共享 epoch 与注册计数，N 个稳定 WaitSlot，通知者扫描并标记一个/全部等待状态。
- Task 执行：统一 run_one，包括 worker、外部 CallerRuns、内部满队列 fallback。

**M1 当前实现边界：**调度只经过固定容量的全局 mutex ring，worker 按 FIFO 取任务；owner TLS 仅用于识别 worker 内满队列提交，并强制 inline fallback。M1 不创建本地 deque、MPMC queue 或 EventCount。worker 等待谓词由 queue mutex 保护的 condition_variable 握手；关闭中 worker 仍可停车等待 pending/gate 归零，不采用 M5 的每 worker WaitSlot/EventCount 注册协议。`DiscardOldest` 在同一个 queue mutex 临界区删除当前可取得的全局头并发布新任务；被移除 Task 在解锁并通知后销毁，因此其 capture destructor 不在队列锁内运行。该 mutex 基线避免 MPMC 中的 reservation retry 窗口，之后替换队列时仍须保留公共所有权和计数协议。

旧 spill_lowest_half、overflow 缓冲、take_overflow、drain_spill_overflow 已删除。固定容量约束由 try_push 返回值表达，没有半批认领后的暂存或失败分区循环。

## 5. 运行机制

### 5.1 获取顺序

本地 pop → 全局 dequeue_bulk → 随机 victim 重复单项 steal → spin → yield → 注册后全 victim 最后探测 → 阻塞。

池层窃取一轮最多 STEAL_CAP 个任务，每个任务有独立 CAS；第一次 steal 失败即停止。接收端必须有足够空间。非 owner 只能 steal，不能向 victim push。

### 5.2 批量接收约束

全局取批仅发生在 owner 的 pop 已返回 nullptr 后。本地没有其他生产者，此时 owner 暂停用户代码，接收量最多 min(BULK_LIMIT, local_capacity)。窃取接收上限 min(STEAL_CAP, local_capacity)，也只在本地空时调用。

批内指针逆序 try_push 保持无窃取/重入交错时的批内 FIFO 执行序。try_push 此时应成功；release 也启用内部 require_invariant（违约记录后 terminate），不能静默丢弃失败项或依赖 debug assert。

通知不负责所有权。外部批量入队仍逐次通知；池层把已认领批转入本地后可补一次通知以激活其他 thief。全池执行序不承诺 FIFO。

### 5.3 worker 骨架

以下取得函数只搬指针，不执行用户代码。full_scan 随机排列全部其他 worker，无单独 victim 数上限，首个认领成功即返回。

~~~cpp
// design-fragment: worker-loop
void ThreadPool::worker_loop(WorkerCtx& ctx) {
    WorkerOwnerScope owner_scope(this, &ctx); // owner 身份与任务执行上下文分离
    for (;;) {
        if (stopping_.load(std::memory_order_seq_cst) &&
            in_flight_submitters_.load(std::memory_order_seq_cst) == 0 &&
            pending_.load(std::memory_order_acquire) == 0) return;

        if (Task* t = acquire_ready(ctx)) { run_one(&ctx, t); continue; }
        if (Task* t = spin_probe(ctx))    { run_one(&ctx, t); continue; }
        if (Task* t = yield_probe(ctx))   { run_one(&ctx, t); continue; }
        if (Task* t = block_on_eventcount(ctx)) run_one(&ctx, t);
    }
}

Task* ThreadPool::block_on_eventcount(WorkerCtx& ctx) {
    const auto key = evc_.prepare_wait();
    evc_.enter(ctx.index);
    // 注册后检查是关闭握手的一部分；不能只检查注册前 stopping。
    if (stopping_.load(std::memory_order_seq_cst)) {
        evc_.leave(ctx.index);
        return nullptr;
    }
    if (Task* t = full_scan(ctx)) {
        evc_.leave(ctx.index);  // 用户代码只能在注销以后执行
        return t;
    }
    evc_.wait_registered(ctx.index, key); // 所有返回路径内部注销
    return nullptr;
}
~~~

spin/yield 只探测本地/全局，探测成功返回 Task*。acquire_ready 从全局取得批或从 victim 逐项窃取后先完成本地转移再 pop 一项。关停期注册后 stopping 检查总会返回，不会重新阻塞；不再依赖有窗口的单次前置检查。

shutdown 后有存量长任务时，其他 worker 暂时忙等。W2.8 以 bench_shutdown_drain 浪费 CPU 数据决定是否增加最终完成通知及关停期停车协议；新协议须重新证明 join 活性。

## 6. 公开 API

### 6.1 基础类型

~~~cpp
// design-fragment: task
class Task {
public:
    virtual ~Task() noexcept = default;
    virtual void run() = 0;
};

template<class F>
class TaskImpl final : public Task {
public:
    template<class G>
        requires std::constructible_from<F, G&&>
    explicit TaskImpl(G&& f) : f_(std::forward<G>(f)) {}
    void run() override { (void)std::invoke(f_); }
private:
    F f_;
};

template<class F>
concept TaskCallable =
    std::constructible_from<std::decay_t<F>, F&&> &&
    std::invocable<std::decay_t<F>&> &&
    std::is_nothrow_destructible_v<std::decay_t<F>>;

enum class RejectionPolicy { CallerRuns, Abort, DiscardOldest };
enum class QueueFullReason { QueueFull, ShuttingDown };

class QueueFullError : public std::runtime_error {
public:
    explicit QueueFullError(QueueFullReason r)
        : std::runtime_error(r == QueueFullReason::QueueFull
                             ? "queue unavailable" : "pool is shutting down"),
          reason(r) {}
    QueueFullReason reason;
};
~~~

Task 是内部执行契约，不提供用户直接提交 Task* 的公开 API。TaskImpl 按 F& 调用存储对象，concept 与 invoke_result 使用同一 value category；只支持 operator()&& 的类型会明确拒绝，operator()& 可接受。decay 只决定存储类型，不能用来证明构造/移动能力。

### 6.2 ThreadPool

~~~cpp
// design-fragment: public-api
struct ThreadPoolOptions {
    std::size_t worker_count = 0;
    std::size_t global_queue_capacity = 4096;
    std::size_t local_deque_capacity = 1024;
    RejectionPolicy rejection = RejectionPolicy::CallerRuns;
    bool pin_to_cores = false;
    std::size_t worker_stack_size = 0;
    std::function<void(std::exception_ptr)> exception_handler;
};

class ThreadPool {
public:
    explicit ThreadPool(const ThreadPoolOptions& = {});
    ~ThreadPool() noexcept;
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template<TaskCallable F>
    void submit(F&& f) {
        submit_task(new TaskImpl<std::decay_t<F>>(std::forward<F>(f)));
    }

    template<TaskCallable F>
    auto submit_async(F&& f)
        -> std::future<std::invoke_result_t<std::decay_t<F>&>> {
        using R = std::invoke_result_t<std::decay_t<F>&>;
        std::packaged_task<R()> pt(std::forward<F>(f));
        auto fut = pt.get_future();
        submit_task(new TaskImpl<std::packaged_task<R()>>(std::move(pt)));
        return fut;
    }

    void shutdown();
    void drain();
    std::size_t worker_count() const noexcept;
    struct Stats {
        std::uint64_t submitted=0, rejected=0, discarded=0, completed=0;
        std::uint64_t worker_completed=0, inline_completed=0, pending=0;
        std::uint64_t stolen=0, local_spills=0, wakes=0;
        std::optional<std::uint64_t> wake_threads;
        std::uint64_t post_wake_hit=0, post_wake_empty=0;
        std::uint64_t pre_sleep_scan_hit=0, pre_sleep_scan_empty=0;
        std::uint64_t notify_scan_slots=0;
    };
    Stats stats() const;
private:
    void submit_task(Task*);
};
~~~

头模板只构造 callable/packaged_task、抽取 future、移交非模板入口。new/构造失败时还未入提交门、未改 pending。future callable 异常由 packaged_task 保存；不再设计无法重新访问 packaged_task 共享状态的 has_future/deliver_exception 兜底。

### 6.3 提交语义

submit 可以内联执行用户代码、分配内存并调用错误回调，不承诺无锁、不阻塞或有界返回。异步仅表示可返回 future，不保证一定在 worker 执行。CallerRuns 的结果在返回前就绪。

shutdown 后直接抛 ShuttingDown，不使用拒绝策略。Abort 对队列当前不可入状态抛 QueueFull；该 reason 表示容量/槽位不可用，可能包含未释放 consumer 槽，不证明逻辑队列严格满。

DiscardOldest 删除当前全局头部可取得任务，最多 DISCARD_RETRY_LIMIT 次；取头失败或重试耗尽抛 QueueFull，不删除本地任务。被丢弃 submit_async 的 future 以 future_errc::broken_promise 就绪；显式 cancellation 异常需另立 shared-state 包装，不属于初版。

### 6.4 等待与执行上下文

本池任务内不得阻塞等待本池 future（包括 get/wait/定时等待），也不得调用本池同步 shutdown/drain/析构。适用于 worker、外部 CallerRuns、内部 fallback、异常回调及池调用的 capture 析构。

`exception_handler` 可并发地从多个 worker 和外部 CallerRuns 提交者调用；调用方必须保证其可变捕获状态线程安全。池不通过额外回调锁串行用户代码。池持有的 handler 在复制及销毁时处于 `ExecutionScope(this)`，并在其他 Impl 同步状态仍存活时销毁；外部 Options 副本的捕获对象仍由调用方管理。

使用独立、可嵌套的 thread_local ExecutionFrame 链表示正在执行的池，遍历链检测本池；不能仅看 owner worker TLS 或最顶层 frame。shutdown/drain 违反契约抛 logic_error；析构违反契约 terminate，release 同样有效。跨池调用只有不形成依赖环才安全，本库不自动检测跨池等待图。

owner TLS 仍只用于本池 deque 快路径，必须同时核对 pool==this。外部 CallerRuns 没有 owner 权限。helping future、非阻塞 request_stop 是未来独立 API；初版不偷偷改变同步 shutdown 含义。

~~~cpp
// design-fragment: execution-scope
class ThreadPool;
struct ExecutionFrame {
    const ThreadPool* pool;
    ExecutionFrame* previous;
};
inline thread_local ExecutionFrame* current_execution = nullptr;

class ExecutionScope {
public:
    explicit ExecutionScope(const ThreadPool* pool) noexcept
        : frame_{pool, current_execution} { current_execution = &frame_; }
    ~ExecutionScope() noexcept { current_execution = frame_.previous; }
    ExecutionScope(const ExecutionScope&) = delete;
    ExecutionScope& operator=(const ExecutionScope&) = delete;
private:
    ExecutionFrame frame_;
};

inline bool executing_pool(const ThreadPool* pool) noexcept {
    for (auto* f = current_execution; f; f = f->previous)
        if (f->pool == pool) return true;
    return false;
}
inline void require_external_control(const ThreadPool* pool) {
    if (executing_pool(pool))
        throw std::logic_error("synchronous control inside this pool task");
}
~~~

shutdown/drain 的首步调用 require_external_control(this)；析构首步检查 executing_pool(this)，true 则 terminate。禁止项以全部执行帧为准，包括跨池嵌套中尚未返回的外层任务。

### 6.5 非目标 API

无优先级、submit_with_timeout、单项 cancellation、helping wait 或快照屏障。需要快照屏障时用独立 ticket/完成水位协议，不把 drain 当作调用时刻快照。

## 7. 关键数据结构

### 7.1 有界 ChaseLevDeque

#### 7.1.1 职责与布局

一个 owner，多个 thief。try_push/pop 仅 owner，steal 任意 worker。固定槽数组在构造后不移动；紧凑 atomic<Task*> 槽，top/bottom 分行。池层最低容量 16，基础结构测试可用容量 4。

#### 7.1.2 实现骨架

normalized_capacity 已经校验为 2 的幂。near-limit 检查在 release 保留；不允许运行期间重置索引。nullptr 不是合法入队载荷。

~~~cpp
// design-fragment: deque
class ChaseLevDeque {
public:
    explicit ChaseLevDeque(std::size_t normalized_capacity)
        : capacity_(normalized_capacity), mask_(normalized_capacity - 1),
          buf_(std::make_unique<std::atomic<Task*>[]>(normalized_capacity)) {}

    std::size_t capacity() const noexcept { return capacity_; }

    bool try_push(Task* x) noexcept {
        require_invariant(x != nullptr);
        const auto b = bottom_.load(std::memory_order_relaxed);
        const auto t = top_.load(std::memory_order_acquire);
        if (b - t >= static_cast<std::int64_t>(capacity_)) return false;
        require_invariant(b < std::numeric_limits<std::int64_t>::max());
        buf_[static_cast<std::size_t>(b) & mask_].store(x, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_release);
        return true;
    }

    Task* pop() noexcept {
        const auto b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto t = top_.load(std::memory_order_relaxed);
        Task* x = nullptr;
        if (t <= b) {
            x = buf_[static_cast<std::size_t>(b) & mask_].load(std::memory_order_relaxed);
            if (t == b) {
                require_invariant(t < std::numeric_limits<std::int64_t>::max());
                if (!top_.compare_exchange_strong(t, t + 1,
                        std::memory_order_seq_cst, std::memory_order_seq_cst))
                    x = nullptr;
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
        } else {
            bottom_.store(b + 1, std::memory_order_relaxed);
        }
        return x;
    }

    Task* steal() noexcept {
        auto t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto b = bottom_.load(std::memory_order_acquire);
        if (t >= b) return nullptr;
        Task* x = buf_[static_cast<std::size_t>(t) & mask_].load(std::memory_order_relaxed);
        require_invariant(t < std::numeric_limits<std::int64_t>::max());
        if (!top_.compare_exchange_strong(t, t + 1,
                std::memory_order_seq_cst, std::memory_order_seq_cst))
            return nullptr;
        return x;
    }

private:
    const std::size_t capacity_, mask_;
    alignas(CACHE_LINE) std::atomic<std::int64_t> top_{0};
    alignas(CACHE_LINE) std::atomic<std::int64_t> bottom_{0};
    std::unique_ptr<std::atomic<Task*>[]> buf_;
};
~~~

普通 pop 没有 top CAS，其线性化依据是 bottom 减量和完整单元素协议；不能把所有 pop 的线性化点写为成功 CAS。稳定边界 invariant 与瞬时中间态分开，见 ADR-001。单元素 steal 因 CAS 失竞争返回 nullptr，不表示所有 victim 确实无任务。

#### 7.1.3 容量与池层 fallback

本地 try_push=false 后，池层单次尝试全局 enqueue 当前 t，成功后通知；失败后内联 run_one。原有本地槽不动，无半批暂存、无 shared deadline、无 TSC 自旋依赖。

批接收 buffer 的容量 BATCH_CAP=max(BULK_LIMIT,STEAL_CAP)，只服务全局接收/重复单项 steal；不再服务 overflow。接收量额外钳制 local_capacity；无重入 user code 时容量证明只需“空 deque + 接收量≤capacity”。

### 7.2 全局 MPMC

#### 7.2.1 载荷

当前 M3 header 原语与编译候选已实现，默认切换仍受平台/性能门禁约束；使用与验证边界见 [MPMC 使用说明](../docs/mpmc-queue.md)。下文保留协议骨架，不将骨架编译当实现验收。

MPMCQueue<T> 存 T；池实例化 MPMCQueue<Task*>，enqueue 接收 Task*，dequeue 通过 Task*& 输出。限定 T 为 trivially copyable 且默认可构造的非抛类型，认领后的数据读写不能抛。

~~~cpp
// design-fragment: mpmc
template<class T>
    requires std::is_trivially_copyable_v<T> &&
             std::is_nothrow_default_constructible_v<T> &&
             std::is_nothrow_copy_constructible_v<T> &&
             std::is_nothrow_copy_assignable_v<T>
class MPMCQueue {
public:
    explicit MPMCQueue(std::size_t normalized_capacity)
        : capacity_(normalized_capacity), mask_(normalized_capacity - 1),
          slots_(std::make_unique<Slot[]>(normalized_capacity)) {
        for (std::size_t i = 0; i < capacity_; ++i)
            slots_[i].seq.store(i, std::memory_order_relaxed);
    }

    bool enqueue(T item) noexcept {
        auto pos = enqueue_pos_.load(std::memory_order_relaxed);
        Slot* slot;
        for (;;) {
            slot = &slots_[pos & mask_];
            const auto seq = slot->seq.load(std::memory_order_acquire);
            // 索引不回绕，上限前 fail-fast，避免有符号减法溢出。
            if (seq == pos) {
                require_index_room(pos, capacity_);
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed)) break;
            } else if (seq < pos) {
                return false; // 当前物理槽不可复用；不一定逻辑满
            } else {
                cpu_relax();
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        slot->data = item;
        slot->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool dequeue(T& item) noexcept {
        auto pos = dequeue_pos_.load(std::memory_order_relaxed);
        Slot* slot;
        for (;;) {
            require_index_room(pos, capacity_);
            slot = &slots_[pos & mask_];
            const auto seq = slot->seq.load(std::memory_order_acquire);
            if (seq == pos + 1) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed)) break;
            } else if (seq < pos + 1) {
                return false; // 头部未发布；后继已发布任务可并存
            } else {
                cpu_relax();
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        item = slot->data;
        slot->seq.store(pos + capacity_, std::memory_order_release);
        return true;
    }

    std::size_t dequeue_bulk(T* out, std::size_t limit) noexcept {
        std::size_t n = 0;
        while (n < limit && dequeue(out[n])) ++n;
        return n;
    }

    bool discard_oldest(T& out) noexcept { return dequeue(out); }

private:
    struct Slot { std::atomic<std::size_t> seq{0}; T data{}; };
    const std::size_t capacity_, mask_;
    std::unique_ptr<Slot[]> slots_;
    alignas(CACHE_LINE) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(CACHE_LINE) std::atomic<std::size_t> dequeue_pos_{0};
};
~~~

require_index_room 确保 pos+capacity 在 size_t 范围内；fail-fast 是 release 的异常状态处理，不是用户配置校验。容量/数组 byte size 的校验在构造前执行。

#### 7.2.2 契约与 progress

先查槽序号，再 CAS 认领；失败返回不携认领。成功后不可抛，release seq 发布/释放、acquire seq 配对。

这不是严格线性化 FIFO 的 try_empty/try_full 契约。成功的 position CAS 定义队列位置顺序，slot release 定义数据何时可访问/复用。不能把 release 发布时刻宣称成与严格 FIFO 相容的全局线性化点。

producer 认领后暂停产生头部 reservation hole；consumer 认领后未释放也会让 producer 暂时不能复用槽。try 返回只保证本次无法取得槽，不保证集合空/满。持续 CAS 重试没有单调用有界完成保证；不将“无 mutex”标为正式 lock-free FIFO progress。已认领线程最终推进是池级进度前提。

#### 7.2.3 bulk 与公平性

bulk 是循环单元素出队，每项独立 CAS；不新增段认领协议。FIFO 只指成功认领的位置顺序，不延伸为发布完成顺序、任务开始/完成顺序。DiscardOldest 只删除当前可取得的头项。

#### 7.2.4 通知

每次新任务成功发布后无条件 notify_one。池层批转移完成后补一次通知。没有近似 depth 抑制；没有注册者仍执行 epoch SC RMW。

### 7.3 EventCount

#### 7.3.1 状态与接口

~~~cpp
// design-fragment: event-count
class EventCount {
public:
    enum : std::uint32_t { Idle = 0, Waiting = 1, Signaled = 2 };
    struct alignas(CACHE_LINE) WaitSlot {
        std::atomic<std::uint32_t> word{Idle};
        #if defined(MAGPIE_GENERIC_BACKEND)
        std::mutex mtx;
        std::condition_variable cv;
        #endif
    };

    explicit EventCount(std::size_t n)
        : count_(n), slots_(std::make_unique<WaitSlot[]>(n)) {}

    std::uint64_t prepare_wait() const noexcept {
        return epoch_.load(std::memory_order_seq_cst);
    }
    void enter(std::size_t i) noexcept {
        require_invariant(i < count_);
        require_invariant(slots_[i].word.load(std::memory_order_seq_cst) == Idle);
        waiters_.fetch_add(1, std::memory_order_seq_cst);
        slots_[i].word.store(Waiting, std::memory_order_seq_cst);
    }
    void leave(std::size_t i) noexcept {
        slots_[i].word.store(Idle, std::memory_order_seq_cst);
        waiters_.fetch_sub(1, std::memory_order_seq_cst);
    }
    void wait_registered(std::size_t i, std::uint64_t key) noexcept {
        if (epoch_.load(std::memory_order_seq_cst) == key)
            backend_wait(slots_[i]); // 只比较 Waiting，不写旧 epoch
        leave(i);
    }
    void notify_one() noexcept {
        advance_epoch();
        if (waiters_.load(std::memory_order_seq_cst) == 0) return;
        const auto start = cursor_.fetch_add(1, std::memory_order_relaxed) % count_;
        for (std::size_t k = 0; k < count_; ++k)
            if (backend_signal(slots_[(start + k) % count_])) return;
    }
    void notify_all() noexcept {
        advance_epoch();
        for (std::size_t i = 0; i < count_; ++i) backend_signal(slots_[i]);
    }

private:
    void advance_epoch() noexcept {
        auto e = epoch_.load(std::memory_order_seq_cst);
        for (;;) {
            require_invariant(e != std::numeric_limits<std::uint64_t>::max());
            if (epoch_.compare_exchange_weak(e, e + 1,
                    std::memory_order_seq_cst, std::memory_order_seq_cst)) return;
        }
    }
    void backend_wait(WaitSlot&) noexcept;
    bool backend_signal(WaitSlot&) noexcept;
    const std::size_t count_;
    std::unique_ptr<WaitSlot[]> slots_;
    alignas(CACHE_LINE) std::atomic<std::uint64_t> epoch_{0};
    alignas(CACHE_LINE) std::atomic<std::size_t> waiters_{0};
    alignas(CACHE_LINE) std::atomic<std::size_t> cursor_{0};
};
~~~

count_ 必须大于 0，构造前校验；cursor 的 unsigned 回绕只影响轮转起点，不属于 generation。worker 唯一管理自己的注册，每 worker 同时最多一次；join 前禁止销毁槽。

#### 7.3.2 平台握手

Linux backend_signal：SC CAS Waiting→Signaled，失败返回 false；成功后 FUTEX_WAKE_PRIVATE 对该槽 wake 1，统计返回值。backend_wait：FUTEX_WAIT_PRIVATE(word,Waiting)，EAGAIN/EINTR/spurious 直接返回；其他错误记录后 terminate。atomic<uint32_t> 的尺寸/对齐/lock-free 与 Linux ABI 在构建时检查。

generic 每槽有自己的 mutex/cv。backend_signal 持该 mutex 执行同一 CAS 并 notify；backend_wait 持同一 mutex 执行谓词 cv.wait(word!=Waiting)。额外 mutex 只保证 generic 的检查到等待原子性，不能替代 Linux 状态机验证。

#### 7.3.3 证明依据

W2 注册先于最后 stop/任务探测；W5 epoch 复查先于阻塞。通知无条件推进 epoch，再选择 Waiting 槽并标记 Signaled。等待者不回写共享字，其他 worker 无法改变本 worker 的状态。

stop 若在注册后的检查之前则被检查拦截；若在之后，epoch 变化或 notify_all 的 Signaled 状态阻止晚睡。详细 SC 顺序、闸门与注销/重新注册边界见 ADR-002。仍需实现与模型检查，不以“全部 SC”冒充完整证明。

#### 7.3.4 成本与移植

Linux futex 和 generic 必须同套语义测试，Linux 另测真实 syscall 窗口。每次通知仍触碰共享 epoch；存在注册者时最多扫描 N 槽，实际 wake 至多一槽，notify_all 最多 N 次 syscall。吞吐及唤醒延迟需测，不预先宣称比 condvar 快。

## 8. 任务生命周期、计数与提交

### 8.1 所有权

模板 new 成功后 submit_task 获得唯一所有权；用 unique_ptr 或等价 RAII 托管至成功发布/内联执行。队列只搬裸指针，不拥有或销毁对象。

执行者 run_one 完整执行、处理异常、delete 后递减 pending。DiscardOldest 先销毁 dropped（其 future broken_promise 就绪），再递减 pending。任何拒绝路径回收包装后回退计数。析构用户 capture 不得抛异常。

### 8.2 pending 与统一递减

~~~cpp
// design-fragment: pending
void ThreadPool::dec_pending() noexcept {
    if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lk(drain_mtx_);
        drain_cv_.notify_all();
    }
}
~~~

pending++ 在队列发布/CallerRuns 前；执行、丢弃、拒绝回退全部走 dec_pending。1→0 的持锁通知与 drain 谓词等待配对。pending 也包含已计数但尚未决定接受的提交，不是队列深度。

### 8.3 任务执行与重入

run_one 建立 ExecutionScope(this)，在同一 scope 内调用 run、池级异常回调、销毁 Task。裸任务异常交给配置回调，未配置则写 stderr；回调再抛则记录并继续。packaged_task 自己保存 callable 异常，无额外 future 投递虚接口。

外部 CallerRuns 统计用 atomic external_stats；worker completed 用自己的 atomic 行。异常处理与销毁完成以后 dec_pending。销毁 rejected/dropped 包装也建立 ExecutionScope(this)，覆盖 pool 调用的用户 capture destructor。

ExecutionFrame 链随 RAII 压入/恢复，所有嵌套帧都参与 require_external_control；异常也正确恢复。owner scope 与 execution scope 分离，跨池嵌套 CallerRuns 不获得对方 deque 权限。

### 8.4 提交准确顺序

1. 获得输入 Task 的 RAII 托管。
2. S0：in_flight_submitters SC++，再读 stopping SC；true 则记录拒绝、销毁并回退门，抛 ShuttingDown。Gate RAII 覆盖所有后续路径。
3. S1：pending release++。
4. 本池 owner：本地 try_push；false 记 local_spills 并尝试全局。成功发布后释放托管、submitted++、notify_one。两个队列均失败则 submitted++、rejected++、run_one 内联配平。
5. 外部：全局 enqueue；成功后释放托管、submitted++、notify_one。
6. 外部 enqueue 失败：rejected++，按配置处理。CallerRuns 算接受，submitted++、run_one。Abort 先销毁、dec_pending 再抛。DiscardOldest 取头成功则销毁旧项、discarded++、dec_pending 后重试；取头失败/重试耗尽销毁新项、dec_pending 再抛。
7. Gate 最后退出，包括 CallerRuns 的任务、回调、销毁和 dec_pending。

所有权发布后的步骤均 noexcept；submitted 在发布后递增允许短暂统计不一致，不再访问 Task。new、错误对象构造或 mutex 异常不能漏门/漏计数；系统级锁错误作为 fail-fast，不能从 worker 逃逸。运行期不得新增可能抛异常的队列内分配。

## 9. 关闭与排空

### 9.1 shutdown

先 require_external_control（遍历本池执行帧，失败抛 logic_error），再按顺序：

1. stopping.store(true,SC)。
2. 自旋/yield 等 in_flight_submitters.load(SC)==0；等待包括已过门 CallerRuns。
3. evc.notify_all()。

以上是目标 EventCount 协议。M1 mutex 变体在同一个 queue mutex 下写入 worker stopping 谓词并通知 queue condition_variable；worker 仅在 stopping、提交门为零、pending 为零同时成立时退出。stopping 时 Gate 的 1→0 与 pending 的 1→0 也在持 queue mutex 时广播；正常运行期间这两个归零转换不广播，任务发布仍只通知一个 worker。该变体允许关闭期停车，不等同于 EventCount 的注册/唤醒握手。M3 MPMC 候选保留该停车协议：worker 常态先无停车锁 dequeue，失败后持锁再次 dequeue/退出裁决；producer seq 发布后取得同一 mutex 再通知，确保 reservation-hole 恢复不会落入探测到 wait 的丢唤醒窗口。

允许多个外部线程调用，重复通知无害。停止后所有 submit 都被拒，包含已有任务再提交子项。任务须自行处理 ShuttingDown。已接受任务仍执行完。

被拒提交仍短暂入门，连续拒绝流可延迟 shutdown 返回，调用方应停止生产；不承诺外部持续调用、无限任务、永久阻塞或线程不被调度时有界完成。同步 shutdown 不能在本池任务/回调中调用，避免自己持 Gate 的自等待。

### 9.2 worker 退出

按 stopping SC → 门 SC 为零 → pending acquire 为零裁决。所有过门提交在 Gate 内计数/发布/执行，读到停止和门零后不会再接受任务；pending 零表示存量清空。后来的拒绝提交不改 pending。

M1 的 condition_variable worker 谓词还将 gate==0 纳入退出条件，避免 worker 在提交者已通过 stop 检查、尚未执行 pending++ 时退出。Gate 1→0 在 stopping 已置位时持 queue mutex 再广播，覆盖这个窗口；pending 1→0 在同一锁上广播以唤醒仍有其他 worker task 的停机 worker。

### 9.3 drain

先 require_external_control，再持 drain_mtx 调用 drain_cv.wait(lk, pending.acquire==0)。保证锚定于**成功观察 pending==0 的时刻**：此前已计数的任务均已执行/销毁或拒绝回退。

持续提交时可以永不返回；读零之后、真正返回之前仍可有新任务。因此不保证返回瞬间集合为空，也不保证“返回前所有入池任务已完成”。shutdown 已结束或调用方保证无并发提交时才能取得排空后保持为空的性质。

### 9.4 析构

析构在本池执行帧中调用则 terminate；否则 shutdown → drain → join 所有 worker。noexcept，join 系统错误记录后 fail-fast。

调用方必须在析构开始前阻止新的成员调用，并保证外部现有成员调用的 lifetime；门协议仅支持存活对象上 submit/shutdown 并发，不能保护模板 new 之前或外部仍持裸 this 的调用。内部 worker 由析构 join 管理。

### 9.5 明确限制

- 本池执行上下文不得同步控制本池生命周期或阻塞等待本池 future。
- CallerRuns 可递归，初版无栈预算，有限任务也可能产生深递归；stack_size 仅缓解，不保证无栈溢出。
- 跨池互等仍可形成依赖环。
- 关停拒绝新的子任务，初版不承诺递归任务树跨 shutdown 完整扩展。
- 生命周期 guard 为 release 有效，不以 debug assert 代替 API 契约。

## 10. 正确性约定与验证

### 10.1 内存序

| 对象 | 基线 |
|---|---|
| deque | §7.1.2 完整单元素协议，双侧 SC fence |
| MPMC pos CAS | relaxed；slot seq acquire/release |
| EventCount epoch/注册/状态 | SC；轮转 cursor relaxed |
| stopping / 提交门 | SC，不放宽 |
| pending | ++ release，-- acq_rel，谓词 acquire |
| stats | relaxed；不建立业务同步 |

任何放宽要求书面模型论证、模型检查、对应测试及 benchmark；TSan/ARM64 压测只是补充证据。

### 10.2 认领与可见性

deque push bottom release；普通 pop bottom 减量按完整协议裁决；最后项 pop/steal top CAS。MPMC 用 position CAS 认领、slot seq 发布/释放，不承诺严格 FIFO 线性化。停止裁决在 stopping store，submit 过门在停止 load；EventCount 的 epoch 和每槽 Signaled 分别承担可见性与停车状态。

### 10.3 审核缺陷与回归

| ID | 缺陷 | 修复机制 | 回归 |
|---|---|---|---|
| R1 | 批量与连续 pop 重复认领 | 单元素 steal，池层重复单项 | StealAcrossOwnerPops |
| R2 | CAS expected 改写导致 bottom 多推 | bottom 恢复 b+1 | LastElementCasFailureRestoresBottom |
| R3 | 单 push 暂存 512 超出 64 | 删除半批暂存；只转投新任务 | LocalOverflowDefaultCapacity |
| R4 | 失败交换后跳项 | 删除失败分区循环 | LocalOverflowTaskConservation |
| R5 | 其他等待者回写共享旧字 | 每 worker 状态字、标记不撤销 | SeparateWaitWords / WakeBeforeKernelWait |
| R6 | stop 检查到注册 TOCTOU | 注册后 stop 复查 + epoch/wake | ShutdownBeforeRegistration / ShutdownAfterRegisteredCheck |
| R7 | CallerRuns 持门/自 pending 等待 | 所有执行帧的控制 guard | CallerRunsControlGuard / NestedExecutionControlGuard |

同时保留提交门、拒绝归零通知、跨池身份、异常回调、discard、构造失败与索引上限专项用例。

### 10.4 矩阵

- 原语：顺序语义、确定性交错、随机 id 守恒、小状态模型检查。
- 池级：提交/拒绝/future、嵌套控制禁令、关闭/排空、构造回滚及配置。
- 工具：release、TSan、ASan+UBSan；完整清单唯一来源 [test-plan](test-plan.md) §6。
- 平台：generic 同套测试为补充，原生 Linux 单独运行真实 futex；ARM64 是内存序放宽的附加必要证据。
- M1 mutex pool 已在 macOS 本机 Release、TSan、ASan+UBSan fast 和 Release 1M ID stress 运行；结果与 Linux/backend 未验收项见 [M1 milestone](../docs/milestones/M1.md)。deque、MPMC、EventCount 正式门禁仍待对应里程碑执行。design/validation 的辅助检查只验证记录的骨架和有限交错。

## 11. 拒绝策略与背压

外部全局 enqueue 不可取得槽才触发配置策略。CallerRuns 提交者执行，生产自然降速，但提交调用可能耗时且实际并发度超过 worker 数。Abort 抛 QueueFull。DiscardOldest 仅删除全局当前可领取头项，存在“删除旧项后新项仍入队失败”的结果，最多两轮。

worker 内本地满不是丢弃/Abort 入口：转投当前新任务，全局仍失败则 CallerRuns。这一内联降级同样记 rejected，旧本地任务不受影响。

容量只约束队列槽，不约束所有同时运行/包装/递归栈内的任务总数。延迟敏感容量取小、突发容量取大只能作为待标定初值，不能承诺容量越小 p99 必然越好。

## 12. 线程模型、配置与构造

### 12.1 配置

worker_count=0 使用 hardware_concurrency；返回 0 时回退 1。显式 worker 数必须在平台和分配上限内。global/local 容量最低 16，校验后向上取 2 的幂；0/过小/取整或 byte multiplication 溢出抛 invalid_argument/length_error，不能依靠 release 消失的 assert。

初版目标 64 位平台。容量限制低于索引上限并检查数组 byte size；所有分配失败正常抛 bad_alloc。底层 normalized_capacity 构造函数为内部 API，公开配置必须先规范化。

~~~cpp
// design-fragment: option-validation
inline std::size_t normalize_capacity(std::size_t requested,
                                     std::size_t minimum,
                                     std::size_t slot_bytes) {
    if (minimum < 2 || slot_bytes == 0 || requested < minimum)
        throw std::invalid_argument("invalid queue capacity");
    const auto limit = std::min<std::size_t>(
        std::numeric_limits<std::size_t>::max() / slot_bytes,
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
    std::size_t cap = 1;
    while (cap < requested) {
        if (cap > limit / 2) throw std::length_error("queue capacity overflow");
        cap *= 2;
    }
    if (cap > limit) throw std::length_error("queue allocation overflow");
    return cap;
}
inline std::size_t effective_worker_count(std::size_t requested,
                                        std::size_t hardware_hint) noexcept {
    return requested ? requested : (hardware_hint ? hardware_hint : 1);
}
~~~

slot_bytes 使用该原语实际 Slot 尺寸（含布局 padding），不得用 Task* 尺寸替代。worker 数也检查平台线程数及 WaitSlot/WorkerCtx/句柄数组的乘法上限。

### 12.2 构造阶段与失败回滚

1. 校验 Options，拷贝回调，构造全局队列/EventCount/统计。
2. 构造全部 WorkerCtx，用 vector<unique_ptr<WorkerCtx>> 保持地址稳定。
3. 启动 worker；线程运行时所有 victim/WaitSlot 已存在。可用启动屏障，但不得访问未构造 victim。
4. 第 k 个启动失败：保存原异常，store stopping → notify_all → join 已建线程 → 销毁上下文 → 重抛。

构造中对象不能泄漏给外部，失败阶段 pending 应为零。绑核/线程名失败记 warning；调度安全不能依赖绑核。

### 12.3 线程后端与亲和性

Linux pthread 后端用 pthread_create(attr, trampoline) 创建线程；worker_stack_size 非零但低于 PTHREAD_STACK_MIN、或 pthread_attr_setstacksize 失败时记录 warning 并退默认。`pin_to_cores` 在成功创建后按允许逻辑 CPU 轮转并 best-effort 调用 pthread_setaffinity_np；cpuset 变化或 affinity 设置失败只 warning，不把 best-effort 绑核变成构造失败。句柄 RAII 只 join 已成功启动的线程，所有控制路径一致。

generic 后端用 std::thread，非零 stack_size warning 忽略。std::thread 不接收 pthread_attr；不再描述在 std::thread 上注入 attr。

默认不强制绑核；hardware_concurrency 是 worker 数量提示，不等于容器 quota。M1 的 `pin_to_cores` 在 Linux 只按进程允许集合中的逻辑 CPU 轮转，不检查物理核拓扑；affinity 查询或线程创建后的 `pthread_setaffinity_np` 失败时记录 warning 并保持不绑核。generic backend 记录 warning 并忽略 pinning。物理核优先、SMT 和超订诊断属于后续有硬件依据的演进；NUMA 仅预留 victim selector 切面。

## 13. 可观测性

Stats 字段：

| 字段 | 口径 |
|---|---|
| submitted | 接受的任务数，**包括 CallerRuns** |
| rejected | 全局不可入/关闭等拒绝处理触发次数，含 CallerRuns；每次提交最多一次 |
| discarded | DiscardOldest 真正销毁的旧任务数 |
| completed | 所有执行完成任务数，含异常和 CallerRuns |
| worker_completed / inline_completed | worker 取队列执行与 submit 内联执行分开；两者和为 completed |
| stolen | 成功单项 steal 的任务数 |
| pending | 未配平计数，包含提交决策中任务 |
| local_spills | 本地 try_push 失败后转投当前任务的次数 |
| wakes / wake_threads | M1 为 condition_variable notify 调用数；目标 backend 调用数 / Linux 实际返回累计；generic 的 wake_threads 为 optional 空 |
| post_wake_hit / post_wake_empty | backend 返回后首轮探测，包含虚假/中断返回 |
| pre_sleep_scan_hit / pre_sleep_scan_empty | 注册后最后探测的命中/空 |
| notify_scan_slots | 通知实际扫描槽数 |

submitted/rejected/discarded 共享行，pending 单独行，门单独行；每 worker completed/stolen 等原子统计分行，external_stats 供多提交者内联完成；epoch/注册/cursor 分行。内联与 worker 执行路径均触碰 pending。

目标 EventCount 版本每次成功提交的共享 RMW 至少：门++/--、pending++、submitted++、epoch推进；完成 pending--。M1 没有 epoch，使用 mutex ring 与 condition_variable；其 `wakes` 记录实际调用 `notify_one`/`notify_all` 的次数，不代表被 OS 唤醒的线程数。stats 按项 relaxed load，允许瞬时不自洽；完全 quiescent、无成员调用时 submitted=completed+discarded 且 pending=0，rejected 不是该恒等式的减项。

Stats 完整定义见 §6.2，计数字段 uint64_t、wake_threads optional<uint64_t>；统计回绕另用明确饱和策略，不用 stats 参与业务协议。inline_completed 包括 worker 线程在 submit 内的 CallerRuns，不能按当前 OS 线程身份将其计入 worker_completed。

## 14. 性能验收

### 14.1 数值门槛

| 指标 | 基准 | 初值 |
|---|---|---|
| 单 worker 空任务吞吐 | bench_empty_task，无 CallerRuns，固定 producer 配置 | ≥1.0×10^6 worker completions/s |
| Worker scaling | bench_worker_scaling，本地非阻塞产生工作，固定 driver，inline 单列 | T8/T1≥6.4，前提供给充足 |
| Skew degradation | bench_skew，相同资源/任务工作量 | 吞吐损失≤20% |
| p99 regression | bench_latency，同日重跑冻结基线 | ≤1.05×且无新峰 |
| 空载 CPU | bench_idle_cpu，10s | ≤3%/worker |
| 对比 speedup | bench_baselines，同资源/同任务数 | vs 显式 launch::async ≥2×；vs 每任务一线程 ≥10× |

worker scaling 测量独立于多 producer 全局提交吞吐；不以增加 producer 或其 CallerRuns 完成数制造 worker 扩展性。W2.6 记录是否因供给不足无法判断，不能把不可用数据当通过。硬件导致门槛调整需双侧数据与理由，不能无证声称目标已达到。

### 14.2 五问

变快的是哪条路径及机制；成本是否转移；是否伤害 p99；是否只在均匀/单线程有效；偏斜扩展性是否仍成立。每组给共享行/alloc/kernel/扫描归因。

### 14.3 负载矩阵

空任务/原子自增/1KB 随机访存/短 sleep；producer 1/2/4/8，worker 1/2/4/8；均匀/单热点/深队列突发/非阻塞嵌套；同资源 mutex 池、显式 launch::async、每任务一线程。

### 14.4 测量纪律

冻结 mutex 基线 commit+binary+checksum+历史数据，正式 A/B 同日重跑 binary；同工具链/配置/机器，≥5 进程重复、预热≥2s、测量≥10s、报告中位数/IQR。完整采集和归因见 benchmark-plan，未执行数据不得记成实证。

## 15. 非目标与已接受限制

优先级、任务图、M:N、CPU/IO 池分离、NUMA 分片、动态 deque、单任务 cancellation、helping wait、任务预算均不在初版。LIFO 在持续新工作下无饥饿上界。CallerRuns 无栈深预算，外部并发执行数不受 worker_count 约束。未来演进不得破坏既有所有权与生命周期契约。

## 16. 工程与演进

### 16.1 工程

C++20，GCC≥11/Clang≥14，release -O2、TSan、ASan+UBSan。测试 GTest 固定 release tag，benchmark 自研 harness；测试与基准不引入生产依赖。

公开薄模板在 thread_pool.h；池主体单独 cpp；deque/MPMC header-only；EventCount 由 linux/generic 平台文件实现。默认 hidden visibility，Task/TaskImpl 模板会引用的公开非模板符号必须导出；不承诺跨版本 ABI。worker thread 后端独立 RAII。

缓存行采用构建宏 MAGPIE_CACHE_LINE，默认 64，允许平台覆盖并校验幂/对齐；hardware_destructive_interference_size 只作为可选探测值，不能使最低工具链无法编译。README 记录 ABI、限制、构建/平台矩阵和许可证。

### 16.2 常量

~~~cpp
// design-fragment: constants
inline constexpr std::size_t CACHE_LINE = 64; // 实现由构建宏覆盖
inline constexpr std::size_t BULK_LIMIT = 32;
inline constexpr std::size_t STEAL_CAP = 64; // 池层重复单项 steal 次数
inline constexpr std::size_t BATCH_CAP = BULK_LIMIT > STEAL_CAP ? BULK_LIMIT : STEAL_CAP;
inline constexpr int SPIN_LIMIT = 64;
inline constexpr int YIELD_LIMIT = 8;
inline constexpr int VICTIM_TRIES = 1;
inline constexpr int DISCARD_RETRY_LIMIT = 2;
~~~

没有 LOCAL_SPIN_US/tsc_now/overflow 常量。require_invariant 与 require_index_room 是 release 生效的 fail-fast；用户 Options 错误以异常报告。标定必须附吞吐/p99、环境和五问。

### 16.3 阶段

W1：mutex 基线先落实 API/所有权/执行上下文/提交门/pending/关闭和统计。W2：MPMC → 单项 deque/池层 fallback → 独立 WaitSlot EventCount，每步独立正确性门禁与 benchmark。W3：只按数据立项进阶功能。

### 16.4 工作项

| ID | 工作项 | 前置与验收 |
|---|---|---|
| W1.1 | CMake/平台/三变体 | 空构建、配置/编译矩阵 |
| W1.2 | Task/薄模板/future/错误 | callable 正反编译、异常与 discard future |
| W1.3 | 提交门/pending/策略 | 拒绝回退/归零通知/守恒 |
| W1.4 | 执行帧、shutdown/drain/析构、构造回滚 | 生命周期 guard、晚注册、部分启动失败 |
| W1.5 | Stats | quiescent 恒等式及 inline/worker 分开 |
| W1.6 | owner TLS 快路径 | 跨池身份、本地满转投、重入 |
| W1.7 | 冻结 mutex 基线 | 测量程序与 binary 归档 |
| W2.1 | MPMC | reservation/consumer hole、不回绕、RelaxedQueueHistory |
| W2.2 | 全局替换 | 同日多 producer 端到端及纯队列数据 |
| W2.3 | 单项 deque | R1/R2 确定性交错、双侧屏障、模型检查 |
| W2.4 | 窃取/fallback | 容量16/1024边界、偏斜数据 |
| W2.5 | 每 worker WaitSlot | R5/R6、原生 Linux syscall、generic补充 |
| W2.6 | 退避/标定/验收 | §14.1六项、独立worker scaling及归因 |
| W2.7 | 放宽验证 | 书面模型、模型检查、ARM64和同日数据 |
| W2.8 | 关停停车演进 | bench_shutdown_drain浪费触发，重证 |
| W2.9 | 槽布局对照 | cache miss/争用/p99双侧数据 |
| W2.10 | 通知扫描/轻量变体 | 扫描/epoch成本触发，重证注册与关闭 |
| W2.11 | helping wait语义草案 | 明确future/控制流/所有权，W3实现 |
| W3.x | allocator、分片、预算、快照屏障等 | 各自独立ADR，不默认排期 |

依赖门禁未通过不得关闭对应工作项；设计辅助检查不等于 W2.3/W2.5 真实实现通过。

## 17. 项目目标

在固定 worker、本地队列与工作窃取基础上降低调度成本，用可证明的所有权与停车协议保证任务守恒和关闭活性，再用真实 benchmark 决定扩展性与尾延迟优化。

## 18. 文档状态

- [ADR-001](chase-lev-deque-adr.md)：v2.0，单项协议与池层 fallback，待实现验证。
- [ADR-002](event-count-adr.md)：v3.0，每 worker 停车字与注册后 stop，待实现与模型检查。
- [ADR-003](mpmc-queue-adr.md)：v2.0，载荷类型/弱 try 契约/进度边界，待实现验证。
- [test-plan](test-plan.md)：正式用例名称、M1 已执行测试及后续原语门禁。
- [benchmark-plan](benchmark-plan.md)：审核修订版，暂无实测数据。
- [审核报告](review-2026-10-03.md)：历史发现，原行号对应修订前内容。
- [修复对照](review-fixes-2026-10-03.md)：逐项修复与有限验证记录。
- [实施步骤与里程碑](implementation-plan.md)：M0–M7 交付、依赖与验收门禁；M0 待 Linux CI，M1 mutex 基线已完成本机门禁，Linux backend 验收待 CI，M2–M7 未开始。

许可证沿用仓库现有 LICENSE，README 作为 W1 交付物补齐。无实测结果时不能以“已接受”表述并发正确性或性能已验收。
