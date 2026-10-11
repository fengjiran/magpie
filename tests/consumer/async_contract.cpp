#include <magpie/thread_pool.hpp>

#include "../support/watchdog.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

magpie::ThreadPoolOptions options(magpie::RejectionPolicy policy) {
    magpie::ThreadPoolOptions result;
    result.worker_count = 1;
    result.global_queue_capacity = 16;
    result.local_deque_capacity = 16;
    result.rejection = policy;
    return result;
}

struct Retirement {
    magpie::ThreadPool* pool = nullptr;
    std::atomic<unsigned> calls{0}, destroyed{0}, guards{0}, counted{0};
    bool throw_on_call = false;
};

struct ObservedCallable {
    Retirement* state;
    explicit ObservedCallable(Retirement& observed) : state(&observed) {}
    ObservedCallable(const ObservedCallable&) = delete;
    ObservedCallable(ObservedCallable&& other) noexcept
        : state(std::exchange(other.state, nullptr)) {}
    ~ObservedCallable() noexcept {
        if (state == nullptr) {
            return;
        }
        if (state->pool->stats().pending != 0) {
            ++state->counted;
        }
        try {
            state->pool->drain();
        } catch (const std::logic_error&) {
            ++state->guards;
        }
        try {
            state->pool->shutdown();
        } catch (const std::logic_error&) {
            ++state->guards;
        }
        ++state->destroyed;
    }
    int operator()() & {
        ++state->calls;
        if (state->throw_on_call) {
            throw std::runtime_error("callable failure");
        }
        return 42;
    }
};

void check_retirement(const Retirement& state, unsigned calls) {
    require(state.destroyed == 1, "future retained the callable");
    require(state.guards == 2, "capture destruction escaped ExecutionScope");
    require(state.counted == 1, "pending retired before the callable");
    require(state.calls == calls, "unexpected callable execution count");
}

void retained_future(bool throws) {
    Retirement state;
    state.throw_on_call = throws;
    std::shared_future<int> future;
    {
        magpie::ThreadPool pool(options(magpie::RejectionPolicy::Abort));
        state.pool = &pool;
        future = pool.submit_async(ObservedCallable(state)).share();
        pool.drain();
        check_retirement(state, 1);
    }
    // Both the result and its retained callback state may outlive the pool.
    if (throws) {
        bool caught = false;
        try {
            (void)future.get();
        } catch (const std::runtime_error&) {
            caught = true;
        }
        require(caught, "callable exception did not reach future");
    } else {
        require(future.get() == 42, "wrong asynchronous result");
    }
    future = {};
    require(state.destroyed == 1, "future release destroyed callable again");
}

struct BlockedPool {
    std::latch started{1}, release{1};
    magpie::ThreadPool pool;
    explicit BlockedPool(magpie::RejectionPolicy policy) : pool(options(policy)) {
        pool.submit([this] {
            started.count_down();
            release.wait();
        });
        started.wait();
    }
    ~BlockedPool() { release.count_down(); }
    void fill(unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            pool.submit([] {});
        }
    }
};

void discarded_future() {
    Retirement state;
    std::future<int> future;
    {
        BlockedPool fixture(magpie::RejectionPolicy::DiscardOldest);
        state.pool = &fixture.pool;
        future = fixture.pool.submit_async(ObservedCallable(state));
        fixture.fill(15);
        fixture.pool.submit([] {});
        check_retirement(state, 0);
        require(future.wait_for(std::chrono::seconds(0)) == std::future_status::ready,
                "discard did not make broken promise ready");
    }
    bool broken = false;
    try {
        (void)future.get();
    } catch (const std::future_error& error) {
        broken = error.code() == std::make_error_code(std::future_errc::broken_promise);
    }
    require(broken, "discard did not preserve broken_promise");
    require(state.destroyed == 1, "discarded future retained ownership");
}

void full_queue(magpie::RejectionPolicy policy) {
    Retirement state;
    BlockedPool fixture(policy);
    state.pool = &fixture.pool;
    fixture.fill(16);
    bool rejected = false;
    std::future<int> future;
    try {
        future = fixture.pool.submit_async(ObservedCallable(state));
    } catch (const magpie::QueueFullError& error) {
        rejected = error.reason == magpie::QueueFullReason::QueueFull;
    }
    const bool inlined = policy == magpie::RejectionPolicy::CallerRuns;
    require(rejected != inlined, "wrong full-queue policy result");
    check_retirement(state, inlined ? 1 : 0);
    if (inlined) {
        require(future.get() == 42, "wrong CallerRuns future result");
    }
}

struct CopyOnly {
    unsigned* copies;
    explicit CopyOnly(unsigned& count) : copies(&count) {}
    CopyOnly(const CopyOnly& other) : copies(other.copies) { ++*copies; }
    CopyOnly(CopyOnly&&) = delete;
    int operator()() & { return 7; }
};

struct ThrowingMove {
    unsigned* moves;
    unsigned throw_at;
    ThrowingMove(unsigned& count, unsigned at) : moves(&count), throw_at(at) {}
    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove(ThrowingMove&& other) : moves(other.moves), throw_at(other.throw_at) {
        if (++*moves == throw_at) {
            throw std::runtime_error("move failure");
        }
    }
    int operator()() & { return 9; }
};

static_assert(magpie::TaskCallable<CopyOnly&>);
static_assert(magpie::TaskCallable<ThrowingMove>);

void callable_types() {
    magpie::ThreadPool pool(options(magpie::RejectionPolicy::Abort));
    unsigned copies = 0;
    CopyOnly copy(copies);
    require(pool.submit_async(copy).get() == 7 && copies == 1,
            "copy-only lvalue was not constructed directly");
    unsigned moves = 0;
    require(pool.submit_async(ThrowingMove(moves, 2)).get() == 9 && moves == 1,
            "async wrapper moved user callable again");
    pool.drain();
    const auto submitted = pool.stats().submitted;
    moves = 0;
    bool caught = false;
    try {
        (void)pool.submit_async(ThrowingMove(moves, 1));
    } catch (const std::runtime_error&) {
        caught = true;
    }
    require(caught && moves == 1, "construction failure did not propagate");
    require(pool.stats().pending == 0 && pool.stats().submitted == submitted,
            "construction failure entered submission accounting");

    pool.submit_async([] {}).get();
    int value = 11;
    require(&pool.submit_async([&]() -> int& { return value; }).get() == &value,
            "reference result identity changed");
    auto owned = pool.submit_async([] { return std::make_unique<int>(13); }).get();
    require(*owned == 13, "move-only result changed");
    pool.drain();
}

}  // namespace

int main() {
    std::atomic<unsigned> phase{0};
    magpie::test::Watchdog watchdog([&] {
        std::fprintf(stderr, "async contract phase=%u\n", phase.load());
    });
    try {
        phase = 1;
        retained_future(false);
        phase = 2;
        retained_future(true);
        phase = 3;
        discarded_future();
        phase = 4;
        full_queue(magpie::RejectionPolicy::Abort);
        phase = 5;
        full_queue(magpie::RejectionPolicy::CallerRuns);
        phase = 6;
        callable_types();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "async contract phase=%u failed: %s\n", phase.load(), error.what());
        return 1;
    }
    std::puts("async ownership, guards, callable types and future results passed");
}
