// main.cpp
// Benchmark / simulation harness.
//
//   Thread 1 (Producer / "market data feed"): generates kNumOrders random
//   crossing limit orders and pushes them into a lock-free SPSC ring buffer
//   as fast as it can.
//
//   Thread 2 (Consumer / matching engine): drains the ring buffer, feeds
//   each order into the OrderBook via match_or_add(), and records the
//   wall-clock cost of that single call with a steady_clock timestamp pair.
//   Latency samples are pre-reserved before the timed region starts, so the
//   *measurement* itself never allocates on the hot path either.
//
// After the run: prints throughput plus mean/p50/p90/p99/p99.9/max latency,
// which is what you'd actually be asked to produce in an HFT interview.
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

namespace {

constexpr std::size_t kNumOrders   = 100'000;
constexpr std::size_t kQueueCap    = 1u << 16; // must be a power of two
constexpr hft::Price  kMinPrice    = 1'800;    // ticks -- centered so buys/sells cross
constexpr hft::Price  kMaxPrice    = 2'200;    // within [0, NumPriceLevels=4096)
constexpr hft::Quantity kMinQty    = 1;
constexpr hft::Quantity kMaxQty    = 500;

using Queue = hft::SPSCQueue<hft::IncomingOrder, kQueueCap>;

void producer(Queue& queue, std::atomic<bool>& done) {
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<std::uint32_t> price_dist(kMinPrice, kMaxPrice);
    std::uniform_int_distribution<std::uint32_t> qty_dist(kMinQty, kMaxQty);
    std::uniform_int_distribution<int>            side_dist(0, 1);
    // 5% market orders, rest limit -- market orders are the "aggressive
    // sweep to exhaustion" edge case that's easy to get wrong.
    std::uniform_int_distribution<int>            type_dist(0, 99);

    for (std::size_t i = 0; i < kNumOrders; ++i) {
        hft::IncomingOrder order{
            .price    = static_cast<hft::Price>(price_dist(rng)),
            .quantity = static_cast<hft::Quantity>(qty_dist(rng)),
            .side     = side_dist(rng) == 0 ? hft::Side::Buy : hft::Side::Sell,
            .type     = type_dist(rng) < 5 ? hft::OrderType::Market : hft::OrderType::Limit,
        };
        // Backpressure: if the ring buffer is momentarily full, spin. In a
        // real feed handler you'd rather spin briefly than drop market data.
        while (!queue.push(order)) {
            std::this_thread::yield();
        }
    }
    done.store(true, std::memory_order_release);
}

struct LatencyStats {
    double mean_ns   = 0;
    std::uint64_t p50_ns  = 0;
    std::uint64_t p90_ns  = 0;
    std::uint64_t p99_ns  = 0;
    std::uint64_t p999_ns = 0;
    std::uint64_t max_ns  = 0;
    std::uint64_t min_ns  = 0;
};

LatencyStats summarize(std::vector<std::uint64_t>& samples) {
    LatencyStats stats{};
    if (samples.empty()) return stats;

    std::sort(samples.begin(), samples.end());
    const std::size_t n = samples.size();

    std::uint64_t sum = 0;
    for (auto v : samples) sum += v;

    stats.mean_ns   = static_cast<double>(sum) / static_cast<double>(n);
    stats.min_ns    = samples.front();
    stats.max_ns    = samples.back();
    stats.p50_ns    = samples[static_cast<std::size_t>(n * 0.50)];
    stats.p90_ns    = samples[static_cast<std::size_t>(n * 0.90)];
    stats.p99_ns    = samples[std::min(n - 1, static_cast<std::size_t>(n * 0.99))];
    stats.p999_ns   = samples[std::min(n - 1, static_cast<std::size_t>(n * 0.999))];
    return stats;
}

} // namespace

int main() {
    // OrderBook<> is ~70MB with default template params (1M-order pool +
    // 4096 price levels on each side) -- heap-allocate the *engine object*
    // itself once at startup (this is the one-time, non-hot-path
    // allocation the spec calls for); every order processed afterwards
    // touches zero allocator calls.
    auto book = std::make_unique<hft::OrderBook<>>();
    Queue queue;
    std::atomic<bool> producer_done{false};

    std::vector<std::uint64_t> latencies_ns;
    latencies_ns.reserve(kNumOrders); // pre-reserved: measurement loop below never allocates

    std::thread producer_thread(producer, std::ref(queue), std::ref(producer_done));

    // Pin-free consumer loop: drain until the producer is done AND the
    // queue is empty (order matters -- check done *before* re-checking
    // empty to avoid a race where we quit with items still in flight).
    hft::IncomingOrder incoming{};
    std::uint64_t total_filled = 0;
    std::size_t processed = 0;

    const auto wall_start = std::chrono::steady_clock::now();

    while (processed < kNumOrders) {
        if (queue.pop(incoming)) {
            const auto t0 = std::chrono::steady_clock::now();
            const hft::ExecReport report = book->match_or_add(incoming);
            const auto t1 = std::chrono::steady_clock::now();

            latencies_ns.push_back(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
            total_filled += report.filled_qty;
            ++processed;
        } else {
            std::this_thread::yield();
        }
    }

    const auto wall_end = std::chrono::steady_clock::now();
    producer_thread.join();

    const double wall_seconds =
        std::chrono::duration<double>(wall_end - wall_start).count();

    const LatencyStats stats = summarize(latencies_ns);

    hft::Price bid_px = 0, ask_px = 0;
    hft::Quantity bid_qty = 0, ask_qty = 0;
    const bool have_bid = book->best_bid(bid_px, bid_qty);
    const bool have_ask = book->best_ask(ask_px, ask_qty);

    std::printf("==================================================================\n");
    std::printf(" Low-Latency Order Book Engine -- Benchmark Results\n");
    std::printf("==================================================================\n");
    std::printf(" Orders processed        : %zu\n", processed);
    std::printf(" Units filled (matched)  : %llu\n", static_cast<unsigned long long>(total_filled));
    std::printf(" Wall time                : %.4f s\n", wall_seconds);
    std::printf(" Throughput               : %.0f orders/sec\n", processed / wall_seconds);
    std::printf(" Orders resting in book   : %zu / %zu pool capacity\n", book->orders_in_use(), book->pool_capacity());
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
    if (have_bid) std::printf(" Best bid: %u ticks x %u qty\n", bid_px, bid_qty);
    else          std::printf(" Best bid: <empty>\n");
    if (have_ask) std::printf(" Best ask: %u ticks x %u qty\n", ask_px, ask_qty);
    else          std::printf(" Best ask: <empty>\n");
    std::printf("==================================================================\n");

    return 0;
}
