// compare_main.cpp
// Single-threaded, apples-to-apples comparison harness: feeds the exact
// same deterministic sequence of orders through either the pooled/bitmap
// engine (include/OrderBook.hpp) or the naive std::map/std::list baseline
// (benchmarks/NaiveOrderBook.hpp), and reports latency percentiles.
//
// Deliberately single-threaded and with no SPSC queue in the loop: the
// point is to isolate the *matching engine's* per-order cost, not the
// producer/consumer pipeline (that's what src/main.cpp measures). Meant to
// be run twice, once per engine, each wrapped in `perf stat` so the
// cache-miss / instruction counts are directly comparable:
//
//   perf stat -e cache-misses,cache-references,instructions,cycles ./compare_bench optimized
//   perf stat -e cache-misses,cache-references,instructions,cycles ./compare_bench naive
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../include/OrderBook.hpp"
#include "NaiveOrderBook.hpp"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace {

constexpr std::size_t kNumOrders = 100'000;
constexpr hft::Price  kMinPrice  = 1'800;
constexpr hft::Price  kMaxPrice  = 2'200;
constexpr hft::Quantity kMinQty  = 1;
constexpr hft::Quantity kMaxQty  = 500;

std::vector<hft::IncomingOrder> generate_orders() {
    std::vector<hft::IncomingOrder> orders;
    orders.reserve(kNumOrders);

    std::mt19937 rng(0xC0FFEE); // fixed seed: identical sequence for both engines
    std::uniform_int_distribution<std::uint32_t> price_dist(kMinPrice, kMaxPrice);
    std::uniform_int_distribution<std::uint32_t> qty_dist(kMinQty, kMaxQty);
    std::uniform_int_distribution<int>            side_dist(0, 1);
    std::uniform_int_distribution<int>            type_dist(0, 99);

    for (std::size_t i = 0; i < kNumOrders; ++i) {
        orders.push_back(hft::IncomingOrder{
            .price    = static_cast<hft::Price>(price_dist(rng)),
            .quantity = static_cast<hft::Quantity>(qty_dist(rng)),
            .side     = side_dist(rng) == 0 ? hft::Side::Buy : hft::Side::Sell,
            .type     = type_dist(rng) < 5 ? hft::OrderType::Market : hft::OrderType::Limit,
        });
    }
    return orders;
}

void pin_to_core(int core) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)core;
#endif
}

struct Stats {
    double mean_ns = 0;
    std::uint64_t p50 = 0, p90 = 0, p99 = 0, max_ns = 0, min_ns = 0;
};

Stats summarize(std::vector<std::uint64_t>& v) {
    Stats s{};
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    std::uint64_t sum = 0;
    for (auto x : v) sum += x;
    s.mean_ns = static_cast<double>(sum) / static_cast<double>(v.size());
    s.min_ns  = v.front();
    s.max_ns  = v.back();
    s.p50     = v[v.size() * 50 / 100];
    s.p90     = v[v.size() * 90 / 100];
    s.p99     = v[std::min(v.size() - 1, v.size() * 99 / 100)];
    return s;
}

template <typename Book>
void run(Book& book, const std::vector<hft::IncomingOrder>& orders, const char* label) {
    std::vector<std::uint64_t> latencies;
    latencies.reserve(orders.size());

    const auto wall_start = std::chrono::steady_clock::now();
    for (const auto& o : orders) {
        const auto t0 = std::chrono::steady_clock::now();
        auto report = book.match_or_add(o);
        const auto t1 = std::chrono::steady_clock::now();
        (void)report;
        latencies.push_back(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    const auto wall_end = std::chrono::steady_clock::now();

    const Stats s = summarize(latencies);
    const double wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();

    std::printf("==================================================================\n");
    std::printf(" %s -- %zu orders\n", label, orders.size());
    std::printf("==================================================================\n");
    std::printf(" wall time     : %.4f s  (%.0f orders/sec)\n", wall_seconds, orders.size() / wall_seconds);
    std::printf(" mean latency  : %.1f ns\n", s.mean_ns);
    std::printf(" min / p50     : %llu / %llu ns\n", (unsigned long long)s.min_ns, (unsigned long long)s.p50);
    std::printf(" p90 / p99     : %llu / %llu ns\n", (unsigned long long)s.p90, (unsigned long long)s.p99);
    std::printf(" max           : %llu ns\n", (unsigned long long)s.max_ns);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || (std::strcmp(argv[1], "optimized") != 0 && std::strcmp(argv[1], "naive") != 0)) {
        std::fprintf(stderr, "usage: %s <optimized|naive>\n", argv[0]);
        return 2;
    }

    pin_to_core(0);

    const auto orders = generate_orders();

    if (std::strcmp(argv[1], "optimized") == 0) {
        auto book = std::make_unique<hft::OrderBook<>>();
        run(*book, orders, "OrderBook (pooled + intrusive lists + summary bitmap)");
    } else {
        hft::naive::NaiveOrderBook book;
        run(book, orders, "NaiveOrderBook (std::map + std::list + unordered_map)");
    }
    return 0;
}
