// MemoryPool.hpp
// A fixed-capacity, array-backed slab allocator for high-performance memory management.
//
// Key Design Goal:
// Dynamic memory allocation (`new`, `delete`, `malloc`, `free`) causes non-deterministic
// latency spikes due to heap management overhead and fragmentation.
//
// MemoryPool pre-allocates a contiguous array of slots at startup.
// Acquiring and releasing slots are O(1) stack push/pop index operations on a free-list,
// ensuring zero dynamic heap allocations during runtime order processing.
#pragma once

#include <array>
#include <cassert>
#include <cstdint>

#include "Types.hpp"

namespace hft {

template <typename T, std::size_t Capacity>
class MemoryPool {
    static_assert(Capacity > 0 && static_cast<std::uint64_t>(Capacity) <= (std::uint64_t{1} << 32),
                  "MemoryPool Capacity must fit within a 32-bit slot index.");

public:
    // Initializes the pool by populating the free-list with available slot indices [0, Capacity - 1].
    MemoryPool() noexcept {
        // Populate the free-list in descending order so that slot 0 is popped first.
        // This produces sequential slot allocation (0, 1, 2, ...) on a fresh pool.
        for (std::size_t index = 0; index < Capacity; ++index) {
            free_slot_list_[index] = static_cast<std::uint32_t>(Capacity - 1 - index);
        }
        available_slots_count_ = Capacity;
    }

    // Disable copy construction and copy assignment to prevent accidental pool duplication.
    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // Acquires a free slot from the pool in O(1) time.
    // Returns the slot index, or kNullIndex if the memory pool is exhausted.
    [[nodiscard]] std::uint32_t acquire() noexcept {
        if (available_slots_count_ == 0) {
            return kNullIndex; // Pool is full: caller handles backpressure/rejection
        }
        // Pop the top available slot index from the free list
        return free_slot_list_[--available_slots_count_];
    }

    // Releases a previously acquired slot back to the free list in O(1) time.
    void release(std::uint32_t slot_index) noexcept {
        assert(available_slots_count_ < Capacity && "MemoryPool::release: Free list overflow (double free error).");
        assert(slot_index < Capacity && "MemoryPool::release: Slot index out of bounds.");

        // Push the released slot index back onto the free list stack
        free_slot_list_[available_slots_count_++] = slot_index;
    }

    // Direct O(1) subscript access to an allocated object in the pool.
    [[nodiscard]] T& operator[](std::uint32_t slot_index) noexcept {
        assert(slot_index < Capacity && "MemoryPool::operator[]: Slot index out of bounds.");
        return storage_slab_[slot_index];
    }

    [[nodiscard]] const T& operator[](std::uint32_t slot_index) const noexcept {
        assert(slot_index < Capacity && "MemoryPool::operator[]: Slot index out of bounds.");
        return storage_slab_[slot_index];
    }

    // Pool capacity and usage statistics
    [[nodiscard]] std::size_t capacity() const noexcept { return Capacity; }
    [[nodiscard]] std::size_t available() const noexcept { return available_slots_count_; }
    [[nodiscard]] std::size_t in_use() const noexcept { return Capacity - available_slots_count_; }

private:
    // Contiguous storage array holding all pre-allocated objects (e.g., Order nodes)
    alignas(64) std::array<T, Capacity> storage_slab_{};

    // Stack storing indices of currently unallocated (free) slots
    alignas(64) std::array<std::uint32_t, Capacity> free_slot_list_{};

    // Number of available free slots remaining in the pool
    std::size_t available_slots_count_ = 0;
};

} // namespace hft

