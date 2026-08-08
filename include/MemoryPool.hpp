// MemoryPool.hpp
// Fixed-capacity, array-backed slab allocator. Every slot is carved out of a
// single contiguous, cache-line-aligned array at startup; acquire()/release()
// are O(1) array-index push/pop on a free-list -- no `new`, `delete`,
// `malloc`, or `std::shared_ptr` ever touch the hot path.
#pragma once

#include <array>
#include <cassert>
#include <cstdint>

#include "Types.hpp"

namespace hft {

template <typename T, std::size_t Capacity>
class MemoryPool {
    static_assert(Capacity > 0 && Capacity <= (std::size_t{1} << 32),
                  "Capacity must fit in a uint32_t slot index");

public:
    MemoryPool() noexcept {
        // free_list_[0] holds the slot that will be handed out *last*; we pop
        // from the back, so push slots on in descending order to acquire 0,1,2,...
        // on a freshly constructed pool (nicer for debugging, not required).
        for (std::size_t i = 0; i < Capacity; ++i) {
            free_list_[i] = static_cast<std::uint32_t>(Capacity - 1 - i);
        }
        free_top_ = Capacity;
    }

    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // O(1). Returns kNullIndex if the pool is exhausted (caller decides how
    // to handle backpressure -- the engine treats it as an order rejection,
    // never blocks or allocates).
    [[nodiscard]] std::uint32_t acquire() noexcept {
        if (free_top_ == 0) [[unlikely]] {
            return kNullIndex;
        }
        return free_list_[--free_top_];
    }

    // O(1). Slot must have come from acquire() on this pool and not have
    // been released twice (checked in debug builds only -- no cost in -O3).
    void release(std::uint32_t slot) noexcept {
        assert(free_top_ < Capacity && "MemoryPool::release: free-list overflow (double free?)");
        assert(slot < Capacity);
        free_list_[free_top_++] = slot;
    }

    [[nodiscard]] T& operator[](std::uint32_t slot) noexcept {
        assert(slot < Capacity);
        return storage_[slot];
    }
    [[nodiscard]] const T& operator[](std::uint32_t slot) const noexcept {
        assert(slot < Capacity);
        return storage_[slot];
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return Capacity; }
    [[nodiscard]] std::size_t available() const noexcept { return free_top_; }
    [[nodiscard]] std::size_t in_use() const noexcept { return Capacity - free_top_; }

private:
    // The slab itself: one contiguous, cache-aligned allocation made once at
    // construction (embedded directly in the OrderBook / engine object, so in
    // practice this lives on the process's static/heap allocation done a
    // single time at startup -- never during order processing).
    alignas(64) std::array<T, Capacity> storage_{};
    alignas(64) std::array<std::uint32_t, Capacity> free_list_{};
    std::size_t free_top_ = 0;
};

} // namespace hft
