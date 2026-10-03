#!/usr/bin/env python3
"""Compile the design fragments and check finite regressions, not the full pool.

Run: python3 design/validation/check_design.py [--sanitize]
Requires Python 3 and a C++20 compiler (CXX or clang++). No third-party libraries.
Linux futex and pool implementation acceptance remain separate requirements.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    design = Path(__file__).resolve().parents[1] / "magpie设计方案.md"
    fragments = {}
    for block in re.findall(r"~~~cpp\n(.*?)\n~~~", design.read_text(), re.S):
        tag = re.search(r"// design-fragment: ([\w-]+)", block)
        if tag:
            fragments[tag.group(1)] = block
    required = {"task", "public-api", "constants", "deque", "mpmc",
                "event-count", "execution-scope", "option-validation",
                "worker-loop", "pending"}
    assert required <= fragments.keys(), required - fragments.keys()
    assert "spill_lowest_half" not in fragments["deque"]
    assert "overflow_" not in fragments["deque"]
    assert "bottom_.store(t + 1" not in fragments["deque"]
    assert "static_cast<std::uint32_t>(key)" not in fragments["event-count"]

    includes = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
'''
    helpers = r'''
inline void require_invariant(bool condition) noexcept {
    if (!condition) std::terminate();
}
inline void require_index_room(std::size_t pos, std::size_t capacity) noexcept {
    require_invariant(capacity <= std::numeric_limits<std::size_t>::max() - pos);
}
inline void cpu_relax() noexcept { std::this_thread::yield(); }
std::function<void(const char*)> active_hook;
void design_hook(const char* name) { if (active_hook) active_hook(name); }
'''
    # Hooks only pause documented protocol boundaries. Production fragments
    # themselves have no callbacks inside their atomic operations.
    deque = fragments["deque"].replace(
        "if (t == b) {", 'if (t == b) {\n                design_hook("pop_last");')
    deque = deque.replace(
        "        require_invariant(t < std::numeric_limits<std::int64_t>::max());\n        if (!top_",
        '        design_hook("steal_candidate");\n'
        "        require_invariant(t < std::numeric_limits<std::int64_t>::max());\n        if (!top_")
    mpmc = fragments["mpmc"].replace(
        "        slot->data = item;", '        design_hook("mpmc_producer_claim");\n        slot->data = item;')
    mpmc = mpmc.replace(
        "        item = slot->data;", '        design_hook("mpmc_consumer_claim");\n        item = slot->data;')
    event = fragments["event-count"].replace(
        "        if (epoch_.load(std::memory_order_seq_cst) == key)\n            backend_wait(slots_[i]);",
        '        if (epoch_.load(std::memory_order_seq_cst) == key) {\n'
        '            design_hook("event_before_wait");\n'
        '            backend_wait(slots_[i]);\n        }')
    assert 'design_hook("steal_candidate")' in deque
    assert 'design_hook("event_before_wait")' in event

    parking = fragments["worker-loop"].split(
        "Task* ThreadPool::block_on_eventcount", 1)[1]
    parking = "Task* block_on_eventcount" + parking
    # The parking fixture carries only the caller's stop flag and notifier.
    # It uses the exact extracted block_on_eventcount body; no full pool stub.
    fixture = """
struct WorkerCtx { std::size_t index; };
struct ParkingFixture {
    std::atomic<bool> stopping_{false};
    EventCount evc_;
    explicit ParkingFixture(std::size_t n) : evc_(n) {}
    Task* full_scan(WorkerCtx&) noexcept { return nullptr; }
""" + parking + "\n};\n"
    backend = r'''
void EventCount::backend_wait(WaitSlot& s) noexcept {
    std::unique_lock lock(s.mtx);
    s.cv.wait(lock, [&] { return s.word.load(std::memory_order_seq_cst) != Waiting; });
}
bool EventCount::backend_signal(WaitSlot& s) noexcept {
    std::lock_guard lock(s.mtx);
    std::uint32_t expected = Waiting;
    if (!s.word.compare_exchange_strong(expected, Signaled,
            std::memory_order_seq_cst, std::memory_order_seq_cst)) return false;
    s.cv.notify_one();
    return true;
}
'''
    tests = r'''
struct IdTask final : Task {
    int id = 0;
    void run() override {}
};
struct LvalueOnly { int operator()() & { return 7; } };
struct RvalueOnly { int operator()() && { return 9; } };
struct MoveOnly {
    std::unique_ptr<int> value = std::make_unique<int>(8);
    int operator()() & { return *value; }
};
static_assert(TaskCallable<LvalueOnly>);
static_assert(!TaskCallable<RvalueOnly>);
static_assert(TaskCallable<MoveOnly>);
static_assert(!TaskCallable<MoveOnly&>);

void passed(const char* name) { std::cout << "PASS " << name << '\n'; }
int main() {
    {
        ChaseLevDeque q(16);
        std::array<IdTask, 8> tasks;
        for (int i = 0; i < 8; ++i) { tasks[i].id = i; assert(q.try_push(&tasks[i])); }
        std::latch candidate(1), resume(1);
        active_hook = [&](const char* tag) {
            if (std::string_view(tag) == "steal_candidate") { candidate.count_down(); resume.wait(); }
        };
        Task* stolen = nullptr;
        std::thread thief([&] { stolen = q.steal(); });
        candidate.wait();
        std::set<Task*> popped;
        for (int i = 0; i < 5; ++i) popped.insert(q.pop());
        resume.count_down(); thief.join(); active_hook = {};
        assert(stolen == &tasks[0] && !popped.contains(stolen));
        assert(q.top_.load() == 1 && q.bottom_.load() == 3);
        assert(q.pop() == &tasks[2]); assert(q.pop() == &tasks[1]); assert(!q.pop());
        passed("R1 StealAcrossOwnerPops");
    }
    {
        ChaseLevDeque q(16); IdTask item;
        assert(q.try_push(&item));
        std::latch candidate(1), resume(1), stolen_done(1);
        active_hook = [&](const char* tag) {
            if (std::string_view(tag) == "steal_candidate") { candidate.count_down(); resume.wait(); }
            if (std::string_view(tag) == "pop_last") { resume.count_down(); stolen_done.wait(); }
        };
        Task* stolen = nullptr;
        std::thread thief([&] { stolen = q.steal(); stolen_done.count_down(); });
        candidate.wait();
        assert(q.pop() == nullptr);
        thief.join(); active_hook = {};
        assert(stolen == &item && q.top_.load() == q.bottom_.load());
        assert(!q.pop()); passed("R2 LastElementCasFailureRestoresBottom");
    }
    {
        ChaseLevDeque q(1024); MPMCQueue<Task*> global(16);
        std::array<IdTask, 1041> tasks;
        for (int i = 0; i < 1024; ++i) assert(q.try_push(&tasks[i]));
        for (int i = 1024; i < 1040; ++i) assert(global.enqueue(&tasks[i]));
        Task* incoming = &tasks[1040];
        assert(!q.try_push(incoming)); assert(!global.enqueue(incoming));
        // Pool fallback executes only this pointer; deque operations already returned.
        incoming->run();
        for (int i = 1023; i >= 0; --i) assert(q.pop() == &tasks[i]);
        assert(!q.pop());
        Task* out = nullptr;
        for (int i = 1024; i < 1040; ++i) { assert(global.dequeue(out)); assert(out == &tasks[i]); }
        assert(!global.dequeue(out));
        passed("R3/R4 FullLocalAndGlobalKeepOldTasks");
    }
    {
        ChaseLevDeque q(16); MPMCQueue<Task*> global(64);
        std::array<IdTask, 32> tasks;
        for (auto& task : tasks) assert(global.enqueue(&task));
        std::array<Task*, BATCH_CAP> batch;
        auto n = global.dequeue_bulk(batch.data(), std::min(BULK_LIMIT, q.capacity()));
        assert(n == 16);
        for (std::size_t i = n; i-- > 0;) assert(q.try_push(batch[i]));
        for (std::size_t i = 0; i < n; ++i) assert(q.pop() == &tasks[i]);
        passed("BulkReceiveCapacityBound");
    }
    {
        EventCount ec(2); auto key = ec.prepare_wait(); ec.enter(0); ec.enter(1);
        active_hook = [&](const char* tag) {
            if (std::string_view(tag) == "event_before_wait") {
                ec.notify_all();
                ec.wait_registered(1, key); // B cancels its own stale registration.
                assert(ec.slots_[0].word.load() == EventCount::Signaled);
            }
        };
        ec.wait_registered(0, key); active_hook = {};
        assert(ec.waiters_.load() == 0);
        passed("R5 SeparateWaitWordsAndWakeBeforeWait");
    }
    {
        ParkingFixture pool(1); WorkerCtx ctx{0};
        pool.stopping_.store(true); pool.evc_.notify_all();
        // Registration starts after shutdown's final notification.
        assert(!pool.block_on_eventcount(ctx));
        assert(pool.evc_.waiters_.load() == 0);
        passed("R6 ShutdownBeforeRegistration");
    }
    {
        ParkingFixture pool(1); WorkerCtx ctx{0};
        active_hook = [&](const char* tag) {
            if (std::string_view(tag) == "event_before_wait") {
                pool.stopping_.store(true); pool.evc_.notify_all();
            }
        };
        assert(!pool.block_on_eventcount(ctx)); active_hook = {};
        assert(pool.evc_.waiters_.load() == 0);
        passed("R6 ShutdownAfterRegisteredCheck");
    }
    {
        ThreadPool a, b; // Identity fixtures, not an implementation of ThreadPool.
        assert(!executing_pool(&a));
        {
            ExecutionScope outer(&a); ExecutionScope inner(&b);
            bool rejected = false;
            try { require_external_control(&a); } catch (const std::logic_error&) { rejected = true; }
            assert(rejected && executing_pool(&a) && executing_pool(&b));
        }
        assert(!executing_pool(&a) && !executing_pool(&b));
        passed("R7 NestedExecutionControlGuard");
    }
    {
        MPMCQueue<int*> q(2); int a=1, b=2; int* out=nullptr;
        std::latch claimed(1), resume(1); std::atomic<bool> first{true};
        active_hook = [&](const char* tag) {
            if (std::string_view(tag)=="mpmc_producer_claim" && first.exchange(false)) {
                claimed.count_down(); resume.wait();
            }
        };
        std::thread producer([&] { assert(q.enqueue(&a)); });
        claimed.wait(); assert(q.enqueue(&b)); assert(!q.dequeue(out));
        resume.count_down(); producer.join(); active_hook = {};
        assert(q.dequeue(out) && out==&a); assert(q.dequeue(out) && out==&b);
        passed("ProducerClaimStallWeakTryContract");
    }
    {
        MPMCQueue<int*> q(2); int a=1,b=2,c=3; int* out=nullptr;
        assert(q.enqueue(&a)); assert(q.enqueue(&b));
        std::latch claimed(1), resume(1); std::atomic<bool> first{true};
        active_hook = [&](const char* tag) {
            if (std::string_view(tag)=="mpmc_consumer_claim" && first.exchange(false)) {
                claimed.count_down(); resume.wait();
            }
        };
        std::thread consumer([&] { int* value=nullptr; assert(q.dequeue(value) && value==&a); });
        claimed.wait(); assert(q.dequeue(out) && out==&b); assert(!q.enqueue(&c));
        resume.count_down(); consumer.join(); active_hook = {};
        assert(q.enqueue(&c)); assert(q.dequeue(out) && out==&c);
        passed("ConsumerClaimStallWeakTryContract");
    }
    {
        assert(effective_worker_count(0,0)==1);
        assert(effective_worker_count(0,8)==8);
        assert(normalize_capacity(17,16,8)==32);
        bool small=false, huge=false;
        try { (void)normalize_capacity(0,16,8); } catch (const std::invalid_argument&) { small=true; }
        try { (void)normalize_capacity(std::numeric_limits<std::size_t>::max(),16,8); }
        catch (const std::length_error&) { huge=true; }
        assert(small && huge); passed("OptionsValidation");
    }
    {
        std::future<int> discarded;
        { std::packaged_task<int()> pt([] { return 42; }); discarded=pt.get_future(); }
        bool broken=false;
        try { (void)discarded.get(); } catch (const std::future_error& e) {
            broken=e.code()==std::make_error_code(std::future_errc::broken_promise);
        }
        assert(broken);
        std::packaged_task<int()> pt(LvalueOnly{}); auto fut=pt.get_future();
        TaskImpl<std::packaged_task<int()>> task(std::move(pt)); task.run();
        assert(fut.get()==7); passed("CallableValueCategoryAndDiscardedFuture");
    }
    {
        constexpr int total=12000;
        MPMCQueue<int*> q(64); std::vector<int> values(total);
        std::vector<std::atomic<int>> seen(total); std::atomic<int> consumed{0};
        for (int i=0; i<total; ++i) values[i]=i;
        std::vector<std::thread> threads;
        for (int p=0;p<3;++p) threads.emplace_back([&,p] {
            for (int i=p;i<total;i+=3) while(!q.enqueue(&values[i])) std::this_thread::yield();
        });
        for (int c=0;c<3;++c) threads.emplace_back([&] {
            while(consumed.load()<total) {
                int* item=nullptr;
                if(q.dequeue(item)) { seen[*item].fetch_add(1); consumed.fetch_add(1); }
                else std::this_thread::yield();
            }
        });
        for(auto& thread:threads) thread.join();
        for(auto& count:seen) assert(count.load()==1);
        passed("MPMCThreadedConservation");
    }
    {
        ParkingFixture pool(4); std::vector<std::thread> workers;
        for(std::size_t i=0;i<4;++i) workers.emplace_back([&,i] {
            WorkerCtx ctx{i};
            while(!pool.stopping_.load()) pool.block_on_eventcount(ctx);
        });
        for(int i=0;i<1000;++i) { pool.evc_.notify_all(); std::this_thread::yield(); }
        pool.stopping_.store(true); pool.evc_.notify_all();
        for(auto& worker:workers) worker.join();
        assert(pool.evc_.waiters_.load()==0);
        passed("GenericConcurrentNotifyAndShutdown");
    }
}
'''
    with tempfile.TemporaryDirectory(prefix="magpie-design-") as tmp:
        root = Path(tmp)
        api = root / "api.cpp"
        api.write_text(includes + fragments["task"] + fragments["public-api"] + r'''
struct OnlyLvalue { int operator()() & { return 7; } };
void instantiate_api(ThreadPool& pool) {
    OnlyLvalue value;
    pool.submit(value);
    auto fut = pool.submit_async(value);
    static_assert(std::same_as<decltype(fut), std::future<int>>);
    pool.submit([p=std::make_unique<int>(8)] { (void)p; });
}
''')
        source = root / "regressions.cpp"
        source.write_text(includes + "#define MAGPIE_GENERIC_BACKEND 1\n" +
                          fragments["constants"] + helpers + fragments["task"] +
                          "class ThreadPool {};\n" + fragments["execution-scope"] +
                          fragments["option-validation"] + "\n#define private public\n" +
                          deque + mpmc + event + "\n#undef private\n" + backend + fixture + tests)
        compiler = shlex.split(os.environ.get("CXX", "clang++"))
        flags = ["-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror"]
        subprocess.run(compiler + flags + ["-fsyntax-only", str(api)], check=True, timeout=60)
        print("PASS PublicApiTemplateCompilation", flush=True)
        if args.sanitize:
            flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        else:
            flags += ["-O2"]
        binary = root / "regressions"
        subprocess.run(compiler + flags + [str(source), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)
    print("Scope: design fragments + generic backend; full pool/Linux futex/ARM64 unverified.")


if __name__ == "__main__":
    main()
