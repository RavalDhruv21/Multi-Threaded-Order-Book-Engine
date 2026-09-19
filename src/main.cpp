// main.cpp
// Multi-threaded Producer-Consumer Benchmark Harness for the HFT Order Book Engine.
//
// Architecture Overview:
// - Producer Thread ("Market Data Feed"):
//   Generates a synthetic stream of crossing Limit and Market orders and pushes them
//   into a lock-free Single-Producer Single-Consumer (SPSC) ring buffer queue.
// - Consumer Thread ("Matching Engine"):
//   Pops orders from the ring buffer, submits each order to `match_or_add()`, and
//   measures high-resolution wall-clock latency per order in nanoseconds.
//
// Performance Results:
// Displays overall throughput (orders/sec) and detailed latency percentiles (p50, p90, p99, p99.9, max).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "OrderBook.hpp"
#include "SPSCQueue.hpp"
#include "Types.hpp"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace {

// Helper function to pin a thread to a specific CPU core (Linux only).
// Prevents thread context switching between cores during benchmark execution.
void pin_thread_to_cpu_core(int core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)core_id;
#endif
}

// Benchmark parameters
constexpr std::size_t kTotalBenchmarkOrders = 100'000;
constexpr std::size_t kRingBufferCapacity    = 1u << 16; // 65,536 power-of-2 capacity
constexpr hft::Price  kMinPriceTick          = 1'800;   // Price range tick values
constexpr hft::Price  kMaxPriceTick          = 2'200;
constexpr hft::Quantity kMinOrderQty        = 1;
constexpr hft::Quantity kMaxOrderQty        = 500;

using OrderRingBuffer = hft::SPSCQueue<hft::IncomingOrder, kRingBufferCapacity>;

// Producer Thread Routine: Generates synthetic orders and pushes into the SPSC queue
void run_producer_feed(OrderRingBuffer& queue, std::atomic<bool>& is_completed) {
    pin_thread_to_cpu_core(0); // Pin producer to core 0

    std::mt19937 random_engine(0xC0FFEE);
    std::uniform_int_distribution<std::uint32_t> price_dist(kMinPriceTick, kMaxPriceTick);
    std::uniform_int_distribution<std::uint32_t> quantity_dist(kMinOrderQty, kMaxOrderQty);
    std::uniform_int_distribution<int>            side_dist(0, 1);
    std::uniform_int_distribution<int>            type_dist(0, 99); // 5% Market orders, 95% Limit orders

    for (std::size_t i = 0; i < kTotalBenchmarkOrders; ++i) {
        const hft::IncomingOrder order{
            .price    = static_cast<hft::Price>(price_dist(random_engine)),
            .quantity = static_cast<hft::Quantity>(quantity_dist(random_engine)),
            .side     = side_dist(random_engine) == 0 ? hft::Side::Buy : hft::Side::Sell,
            .type     = type_dist(random_engine) < 5 ? hft::OrderType::Market : hft::OrderType::Limit,
        };

        // If queue is full, yield CPU briefly until space is available
        while (!queue.push(order)) {
            std::this_thread::yield();
        }
    }
    is_completed.store(true, std::memory_order_release);
}

// Latency summary statistics structure
struct LatencyStatistics {
    double        mean_ns   = 0.0;
    std::uint64_t min_ns    = 0;
    std::uint64_t max_ns    = 0;
    std::uint64_t p50_ns    = 0;
    std::uint64_t p90_ns    = 0;
    std::uint64_t p99_ns    = 0;
    std::uint64_t p999_ns   = 0;
};

// Calculates latency percentiles from measured nanosecond samples
LatencyStatistics calculate_latency_stats(std::vector<std::uint64_t>& samples) {
    LatencyStatistics stats{};
    if (samples.empty()) return stats;

    std::sort(samples.begin(), samples.end());
    const std::size_t count = samples.size();

    std::uint64_t total_latency_sum = 0;
    for (const auto sample : samples) {
        total_latency_sum += sample;
    }

    stats.mean_ns = static_cast<double>(total_latency_sum) / static_cast<double>(count);
    stats.min_ns  = samples.front();
    stats.max_ns  = samples.back();
    stats.p50_ns  = samples[static_cast<std::size_t>(count * 0.50)];
    stats.p90_ns  = samples[static_cast<std::size_t>(count * 0.90)];
    stats.p99_ns  = samples[std::min(count - 1, static_cast<std::size_t>(count * 0.99))];
    stats.p999_ns = samples[std::min(count - 1, static_cast<std::size_t>(count * 0.999))];
    return stats;
}

} // namespace

int main() {
    // Heap-allocate large OrderBook engine object (~70MB slab pool) once at startup
    auto matching_engine = std::make_unique<hft::OrderBook<>>();
    OrderRingBuffer ring_buffer;
    std::atomic<bool> producer_done{false};

    // Pre-reserve latency vector capacity to eliminate allocations during measurement loop
    std::vector<std::uint64_t> latency_samples_ns;
    latency_samples_ns.reserve(kTotalBenchmarkOrders);

    // Spawn producer thread
    std::thread producer_thread(run_producer_feed, std::ref(ring_buffer), std::ref(producer_done));
    pin_thread_to_cpu_core(1); // Pin consumer matching engine to core 1

    hft::IncomingOrder incoming_order{};
    std::uint64_t total_units_filled = 0;
    std::size_t processed_orders_count = 0;

    const auto benchmark_start_time = std::chrono::steady_clock::now();

    // Consumer loop: Drain order ring buffer and measure matching execution time
    while (processed_orders_count < kTotalBenchmarkOrders) {
        if (ring_buffer.pop(incoming_order)) {
            const auto t_start = std::chrono::steady_clock::now();
            const hft::ExecReport report = matching_engine->match_or_add(incoming_order);
            const auto t_end   = std::chrono::steady_clock::now();

            const auto latency_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count()
            );
            latency_samples_ns.push_back(latency_ns);
            total_units_filled += report.filled_qty;
            ++processed_orders_count;
        } else {
            std::this_thread::yield();
        }
    }

    const auto benchmark_end_time = std::chrono::steady_clock::now();
    producer_thread.join();

    const double elapsed_seconds = std::chrono::duration<double>(benchmark_end_time - benchmark_start_time).count();
    const LatencyStatistics stats = calculate_latency_stats(latency_samples_ns);

    // Query best bid and ask levels
    hft::Price best_bid_price = 0, best_ask_price = 0;
    hft::Quantity best_bid_qty = 0, best_ask_qty = 0;
    const bool has_bid = matching_engine->best_bid(best_bid_price, best_bid_qty);
    const bool has_ask = matching_engine->best_ask(best_ask_price, best_ask_qty);

    // Print summary benchmark report
    std::printf("==================================================================\n");
    std::printf(" Low-Latency Order Book Engine -- Benchmark Results\n");
    std::printf("==================================================================\n");
    std::printf(" Orders processed        : %zu\n", processed_orders_count);
    std::printf(" Units filled (matched)  : %llu\n", static_cast<unsigned long long>(total_units_filled));
    std::printf(" Wall time                : %.4f s\n", elapsed_seconds);
    std::printf(" Throughput               : %.0f orders/sec\n", processed_orders_count / elapsed_seconds);
    std::printf(" Orders resting in book   : %zu / %zu pool capacity\n", matching_engine->orders_in_use(), matching_engine->pool_capacity());
    std::printf("------------------------------------------------------------------\n");
    std::printf(" Per-order match_or_add() latency (ns):\n");
    std::printf("   mean   : %.1f\n", stats.mean_ns);
    std::printf("   min    : %llu\n", static_cast<unsigned long long>(stats.min_ns));
    std::printf("   p50    : %llu\n", static_cast<unsigned long long>(stats.p50_ns));
    std::printf("   p90    : %llu\n", static_cast<unsigned long long>(stats.p90_ns));
    std::printf("   p99    : %llu\n", static_cast<unsigned long long>(stats.p99_ns));
    std::printf("   p99.9  : %llu\n", static_cast<unsigned long long>(stats.p999_ns));
    std::printf("   max    : %llu\n", static_cast<unsigned long long>(stats.max_ns));
    std::printf("------------------------------------------------------------------\n");
    if (has_bid) std::printf(" Best bid: %u ticks x %u qty\n", best_bid_price, best_bid_qty);
    else         std::printf(" Best bid: <empty>\n");
    if (has_ask) std::printf(" Best ask: %u ticks x %u qty\n", best_ask_price, best_ask_qty);
    else         std::printf(" Best ask: <empty>\n");
    std::printf("==================================================================\n");

    return 0;
}

