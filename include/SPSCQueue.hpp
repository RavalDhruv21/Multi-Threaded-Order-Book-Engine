// SPSCQueue.hpp
// Single-Producer/Single-Consumer lock-free ring buffer. One thread calls
// push(), a different single thread calls pop(); no other thread ever
// touches either. That constraint (vs MPMC) is what lets us get away with
// two plain atomics and no CAS loop at all.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace hft {

// Padding between head_ and tail_ so the producer's writes to tail_ and the
// consumer's writes to head_ never land on the same cache line (false
// sharing would otherwise bounce that line between cores on every single
// push/pop, which is the whole "lock-free but still slow" trap). Hardcoded
// to 64 rather than std::hardware_destructive_interference_size: that
// constant is explicitly not ABI-stable across compilers/-mtune flags
// (GCC warns on use), and 64 bytes is correct for every mainstream x86-64
// and AArch64 target this project builds for.
inline constexpr std::size_t kCacheLine = 64;

template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two (mask-based wraparound relies on it)");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCQueue is memcpy-semantics only -- no allocation, no destructors on the hot path");

public:
    SPSCQueue() = default;
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // Producer side only. Returns false if the queue is full (backpressure
    // signal -- caller decides whether to spin, drop, or retry).
    bool push(const T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & kMask;

        // Acquire here pairs with the consumer's release store to head_:
        // if we observe the consumer has moved head_ far enough for the
        // slot to be free, we are guaranteed to also see everything the
        // consumer did with that slot before publishing head_.
        if (next == head_.load(std::memory_order_acquire)) {
            return false; // full
        }

        buffer_[tail] = item;

        // Release here pairs with the consumer's acquire load of tail_:
        // publishes both the new tail index *and* the just-written element
        // to the consumer.
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side only. Returns false if the queue is empty.
    bool pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        if (head == tail_.load(std::memory_order_acquire)) {
            return false; // empty
        }

        out = buffer_[head];
        head_.store((head + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    alignas(kCacheLine) std::array<T, Capacity> buffer_{};

    // head_ and tail_ each get their own cache line. Without this padding,
    // a producer spinning on tail_ and a consumer spinning on head_ would
    // constantly invalidate each other's core-local cache line even though
    // they're logically touching "different" atomics.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
};

} // namespace hft
