#pragma once

#include <magpie/build_config.hpp>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
#include <magpie/detail/mpmc_test_hooks.hpp>
#endif

namespace magpie {

template <class T>
concept MpmcPayload =
    std::is_trivially_copyable_v<T> && std::is_nothrow_default_constructible_v<T> &&
    std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_assignable_v<T>;

// Failed try operations mean unavailable, not a strict empty/full observation.
// Only seq transfers access to ordinary data; position CAS reserves a ticket.
template <MpmcPayload T> class MPMCQueue final {
    struct Slot {
        std::atomic<std::size_t> seq{0};
        T data{};
    };

  public:
    static_assert(sizeof(std::size_t) == 8, "MPMC requires a 64-bit target");
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static constexpr std::size_t slot_size = sizeof(Slot);

    explicit MPMCQueue(std::size_t capacity
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
                       ,
                       const detail::MpmcTestHooks* hooks = nullptr
#endif
                       )
        : capacity_(validate_capacity(capacity)), mask_(capacity_ - 1),
          slots_(std::make_unique<Slot[]>(capacity_))
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
          ,
          hooks_(hooks)
#endif
    {
        for (std::size_t i = 0; i < capacity_; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }
    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    bool enqueue(T item) noexcept {
        if constexpr (std::is_pointer_v<T>) {
            if (item == nullptr) {
                fail("null MPMC payload");
            }
        }
        auto pos = enqueue_pos_.load(std::memory_order_relaxed);
        Slot* slot;
        for (;;) {
            slot = &slots_[pos & mask_];
            const auto seq = slot->seq.load(std::memory_order_acquire);
            if (seq == pos) {
                require_index_room(pos);
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (seq < pos) {
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
        if (hooks_ && hooks_->after_enqueue_claim) {
            hooks_->after_enqueue_claim(pos, hooks_->context);
        }
#endif
        slot->data = item;
        slot->seq.store(pos + 1, std::memory_order_release);
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
        if (hooks_ && hooks_->after_enqueue_publish) {
            hooks_->after_enqueue_publish(pos, hooks_->context);
        }
#endif
        return true;
    }

    bool dequeue(T& item) noexcept {
        auto pos = dequeue_pos_.load(std::memory_order_relaxed);
        Slot* slot;
        for (;;) {
            require_index_room(pos);
            slot = &slots_[pos & mask_];
            const auto seq = slot->seq.load(std::memory_order_acquire);
            if (seq == pos + 1) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (seq < pos + 1) {
                return false;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
        if (hooks_ && hooks_->after_dequeue_claim) {
            hooks_->after_dequeue_claim(pos, hooks_->context);
        }
#endif
        item = slot->data;
        slot->seq.store(pos + capacity_, std::memory_order_release);
        return true;
    }

    std::size_t dequeue_bulk(T* out, std::size_t limit) noexcept {
        if (out == nullptr && limit != 0) {
            fail("null MPMC bulk output");
        }
        std::size_t n = 0;
        while (n < limit && dequeue(out[n])) {
            ++n;
        }
        return n;
    }
    bool discard_oldest(T& out) noexcept { return dequeue(out); }

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    std::array<std::size_t, 2> positions_for_test() const noexcept {
        return {enqueue_pos_.load(std::memory_order_relaxed),
                dequeue_pos_.load(std::memory_order_relaxed)};
    }
    void dump_for_test() const noexcept {
        const auto positions = positions_for_test();
        std::fprintf(stderr, "MPMC watchdog: enqueue=%zu dequeue=%zu capacity=%zu\n", positions[0],
                     positions[1], capacity_);
        for (std::size_t i = 0; i < capacity_; ++i) {
            std::fprintf(stderr, "slot[%zu].seq=%zu\n", i,
                         slots_[i].seq.load(std::memory_order_relaxed));
        }
    }
    // Quiescent test fixture only. Every physical slot starts at its next ticket.
    void initialize_empty_at_for_test(std::size_t position) noexcept {
        if (position > std::numeric_limits<std::size_t>::max() - (capacity_ - 1)) {
            fail("invalid test position");
        }
        enqueue_pos_.store(position, std::memory_order_relaxed);
        dequeue_pos_.store(position, std::memory_order_relaxed);
        for (std::size_t offset = 0; offset < capacity_; ++offset) {
            slots_[(position + offset) & mask_].seq.store(position + offset,
                                                          std::memory_order_relaxed);
        }
    }
#endif
  private:
    static std::size_t validate_capacity(std::size_t capacity) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("MPMC capacity must be a power of two >= 2");
        }
        if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(Slot) ||
            capacity > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::length_error("MPMC slot allocation overflow");
        }
        return capacity;
    }
    [[noreturn]] static void fail(const char* message) noexcept {
        std::fprintf(stderr, "magpie: %s\n", message);
        std::terminate();
    }
    void require_index_room(std::size_t position) const noexcept {
        if (position > std::numeric_limits<std::size_t>::max() - capacity_) {
            fail("MPMC index limit");
        }
    }
    const std::size_t capacity_, mask_;
    std::unique_ptr<Slot[]> slots_;
    alignas(MAGPIE_CACHE_LINE) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(MAGPIE_CACHE_LINE) std::atomic<std::size_t> dequeue_pos_{0};
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    const detail::MpmcTestHooks* hooks_;
#endif
};
} // namespace magpie
