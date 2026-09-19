// SPSCQueue.hpp
// Single-Producer / Single-Consumer (SPSC) Lock-Free Ring Buffer.
//
// Key Concurrency Design:
// This queue allows thread-safe communication between exactly two threads:
//   - Producer Thread: Pushes items into the queue (calls `push`).
//   - Consumer Thread: Pops items out of the queue (calls `pop`).
//
// Because only ONE thread writes to the write index (tail) and only ONE thread
// writes to the read index (head), mutex locks or compare-and-swap (CAS) loops are NOT required.
// Plain atomic memory barriers (`acquire` and `release`) guarantee lock-free performance.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace hft {

// Standard cache line size in bytes (64 bytes for x86-64 and ARM64 processors).
// Used to align memory structures and avoid cache line false-sharing between threads.
constexpr std::size_t kCacheLineBytes = 64;

template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "SPSCQueue Capacity must be a power of 2 (e.g., 64, 1024, 65536).");
    static_assert(std::is_trivially_copyable<T>::value,
                  "SPSCQueue elements must be trivially copyable for memcpy transfer.");

public:
    SPSCQueue() = default;

    // Prevent copying queue instances
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // Pushes an item into the queue. Executed by the PRODUCER thread ONLY.
    // Returns false if the queue is full (backpressure signal).
    bool push(const T& item) noexcept {
        const std::size_t current_tail = tail_index_.load(std::memory_order_relaxed);
        const std::size_t next_tail    = (current_tail + 1) & index_mask_;

        // Check if queue is full: pairing acquire load with consumer's release store to head_index_
        if (next_tail == head_index_.load(std::memory_order_acquire)) {
            return false; // Queue is full
        }

        // Store item in ring buffer slot
        buffer_[current_tail] = item;

        // Publish updated tail index to consumer using release store
        tail_index_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Pops an item from the queue. Executed by the CONSUMER thread ONLY.
    // Returns false if the queue is empty.
    bool pop(T& output_item) noexcept {
        const std::size_t current_head = head_index_.load(std::memory_order_relaxed);

        // Check if queue is empty: pairing acquire load with producer's release store to tail_index_
        if (current_head == tail_index_.load(std::memory_order_acquire)) {
            return false; // Queue is empty
        }

        // Retrieve item from ring buffer slot
        output_item = buffer_[current_head];

        // Advance head index and publish to producer using release store
        const std::size_t next_head = (current_head + 1) & index_mask_;
        head_index_.store(next_head, std::memory_order_release);
        return true;
    }

    // Checks if the queue is empty
    [[nodiscard]] bool empty() const noexcept {
        return head_index_.load(std::memory_order_acquire) == tail_index_.load(std::memory_order_acquire);
    }

    // Returns maximum capacity of the ring buffer
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // Bitmask for fast modulo wrapping (Capacity must be a power of 2)
    static constexpr std::size_t index_mask_ = Capacity - 1;

    // Ring buffer storage array aligned to cache line boundary
    alignas(kCacheLineBytes) std::array<T, Capacity> buffer_{};

    // Read index (head): Owned and modified primarily by the Consumer thread.
    // Separated onto its own cache line to prevent false sharing with tail_index_.
    alignas(kCacheLineBytes) std::atomic<std::size_t> head_index_{0};

    // Write index (tail): Owned and modified primarily by the Producer thread.
    // Separated onto its own cache line to prevent false sharing with head_index_.
    alignas(kCacheLineBytes) std::atomic<std::size_t> tail_index_{0};
};

} // namespace hft

