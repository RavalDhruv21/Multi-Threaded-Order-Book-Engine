// test_orderbook.cpp
// Lightweight, dependency-free functional test suite (no gtest -- kept
// self-contained so `g++ -O2 -std=c++20 tests/test_orderbook.cpp -o test`
// just works). Exercises the correctness properties an interviewer will
// actually probe: price-time priority, partial fills, market order sweep,
// O(1) cancel, amend semantics, and stale-handle rejection.
#include <cstdio>
#include <cstdlib>

#include "OrderBook.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                               \
        } else {                                                                        \
            std::printf("  ok: %s\n", #cond);                                           \
        }                                                                               \
    } while (0)

using hft::IncomingOrder;
using hft::OrderType;
using hft::Side;

// Small book for tests: fewer price levels/orders so failures are easy to reason about.
using TestBook = hft::OrderBook<1024, 4096>;

void test_resting_and_best_price() {
    std::printf("-- test_resting_and_best_price --\n");
    TestBook book;
    auto r1 = book.match_or_add({.price = 100, .quantity = 10, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(r1.accepted);
    CHECK(r1.filled_qty == 0);
    CHECK(r1.remaining_qty == 10);

    auto r2 = book.match_or_add({.price = 105, .quantity = 5, .side = Side::Buy, .type = OrderType::Limit});
    (void)r2;

    hft::Price px{}; hft::Quantity qty{};
    CHECK(book.best_bid(px, qty));
    CHECK(px == 105); // higher bid should be best
    CHECK(qty == 5);
}

void test_crossing_full_fill() {
    std::printf("-- test_crossing_full_fill --\n");
    TestBook book;
    book.match_or_add({.price = 100, .quantity = 10, .side = Side::Sell, .type = OrderType::Limit});
    auto r = book.match_or_add({.price = 100, .quantity = 10, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(r.filled_qty == 10);
    CHECK(r.remaining_qty == 0);
    CHECK(r.order_id == 0); // fully filled, nothing rests

    hft::Price px{}; hft::Quantity qty{};
    CHECK(!book.best_ask(px, qty)); // ask side should now be empty
}

void test_partial_fill_and_price_time_priority() {
    std::printf("-- test_partial_fill_and_price_time_priority --\n");
    TestBook book;
    // Two sell orders at the same price -- FIFO: the first one in should be
    // consumed first.
    auto s1 = book.match_or_add({.price = 100, .quantity = 5, .side = Side::Sell, .type = OrderType::Limit});
    auto s2 = book.match_or_add({.price = 100, .quantity = 5, .side = Side::Sell, .type = OrderType::Limit});
    CHECK(s1.accepted && s2.accepted);

    // Buy 7: should fully consume s1 (5) and partially consume s2 (2),
    // leaving s2 resting with qty 3.
    auto b = book.match_or_add({.price = 100, .quantity = 7, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(b.filled_qty == 7);
    CHECK(b.remaining_qty == 0);

    hft::Price px{}; hft::Quantity qty{};
    CHECK(book.best_ask(px, qty));
    CHECK(px == 100);
    CHECK(qty == 3); // remainder of s2
}

void test_market_order_sweeps_multiple_levels() {
    std::printf("-- test_market_order_sweeps_multiple_levels --\n");
    TestBook book;
    book.match_or_add({.price = 100, .quantity = 5, .side = Side::Sell, .type = OrderType::Limit});
    book.match_or_add({.price = 101, .quantity = 5, .side = Side::Sell, .type = OrderType::Limit});

    // Market buy for 8: should take all 5 @100 then 3 @101, ignoring price.
    auto r = book.match_or_add({.price = 0, .quantity = 8, .side = Side::Buy, .type = OrderType::Market});
    CHECK(r.filled_qty == 8);
    CHECK(r.remaining_qty == 0);

    hft::Price px{}; hft::Quantity qty{};
    CHECK(book.best_ask(px, qty));
    CHECK(px == 101);
    CHECK(qty == 2);
}

void test_market_order_ioc_drops_remainder() {
    std::printf("-- test_market_order_ioc_drops_remainder --\n");
    TestBook book;
    book.match_or_add({.price = 100, .quantity = 3, .side = Side::Sell, .type = OrderType::Limit});

    auto r = book.match_or_add({.price = 0, .quantity = 10, .side = Side::Buy, .type = OrderType::Market});
    CHECK(r.filled_qty == 3);
    CHECK(r.remaining_qty == 7);   // unfilled remainder exists...
    CHECK(r.order_id == 0);        // ...but is NOT resting in the book (IOC)

    hft::Price px{}; hft::Quantity qty{};
    CHECK(!book.best_bid(px, qty));
}

void test_cancel_is_o1_and_removes_liquidity() {
    std::printf("-- test_cancel_is_o1_and_removes_liquidity --\n");
    TestBook book;
    auto r = book.match_or_add({.price = 100, .quantity = 10, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(r.order_id != 0);

    hft::Price px{}; hft::Quantity qty{};
    CHECK(book.best_bid(px, qty));

    CHECK(book.cancel_order(r.order_id));
    CHECK(!book.best_bid(px, qty)); // level should be empty now

    // Double-cancel must fail cleanly, not crash / not double-release the slot.
    CHECK(!book.cancel_order(r.order_id));
}

void test_stale_handle_after_slot_reuse_is_rejected() {
    std::printf("-- test_stale_handle_after_slot_reuse_is_rejected --\n");
    TestBook book;
    auto r1 = book.match_or_add({.price = 100, .quantity = 1, .side = Side::Buy, .type = OrderType::Limit});
    const hft::OrderId stale_id = r1.order_id;
    CHECK(book.cancel_order(stale_id)); // slot freed, generation bumped

    // Force the same slot to be reused by placing enough new orders that at
    // least one reuses slot 0 (pool is LIFO free-list, so the very next
    // acquire reuses exactly the slot we just freed).
    auto r2 = book.match_or_add({.price = 200, .quantity = 1, .side = Side::Buy, .type = OrderType::Limit});
    (void)r2;

    // The old handle must NOT be able to cancel the new order occupying the
    // recycled slot -- this is exactly the ABA bug generation counters exist
    // to prevent.
    CHECK(!book.cancel_order(stale_id));

    hft::Price px{}; hft::Quantity qty{};
    CHECK(book.best_bid(px, qty));
    CHECK(px == 200); // the new order must still be resting, untouched
}

void test_amend_quantity_decrease_keeps_priority() {
    std::printf("-- test_amend_quantity_decrease_keeps_priority --\n");
    TestBook book;
    auto r1 = book.match_or_add({.price = 100, .quantity = 10, .side = Side::Sell, .type = OrderType::Limit});
    auto r2 = book.match_or_add({.price = 100, .quantity = 10, .side = Side::Sell, .type = OrderType::Limit});

    // Shrink r1's quantity in place (should keep FIFO position: still first).
    auto amend_result = book.amend_order(r1.order_id, 100, 4);
    CHECK(amend_result.accepted);

    // A buy for 4 should be filled entirely by (amended) r1, not r2.
    auto b = book.match_or_add({.price = 100, .quantity = 4, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(b.filled_qty == 4);

    hft::Price px{}; hft::Quantity qty{};
    CHECK(book.best_ask(px, qty));
    CHECK(qty == 10); // r2 fully untouched
    (void)r2;
}

void test_amend_price_change_reprioritizes() {
    std::printf("-- test_amend_price_change_reprioritizes --\n");
    TestBook book;
    auto r = book.match_or_add({.price = 100, .quantity = 5, .side = Side::Buy, .type = OrderType::Limit});
    auto amend_result = book.amend_order(r.order_id, 110, 5);
    CHECK(amend_result.accepted);

    hft::Price px{}; hft::Quantity qty{};
    CHECK(book.best_bid(px, qty));
    CHECK(px == 110); // moved to the new price level
}

void test_pool_exhaustion_is_handled_gracefully() {
    std::printf("-- test_pool_exhaustion_is_handled_gracefully --\n");
    hft::OrderBook<4, 4096> tiny_book; // capacity for only 4 resting orders
    for (int i = 0; i < 4; ++i) {
        auto r = tiny_book.match_or_add(
            {.price = static_cast<hft::Price>(100 + i), .quantity = 1, .side = Side::Buy, .type = OrderType::Limit});
        CHECK(r.accepted);
    }
    // 5th order: pool is exhausted, must be rejected, not crash / UB.
    auto r5 = tiny_book.match_or_add({.price = 200, .quantity = 1, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(!r5.accepted);
}

} // namespace

int main() {
    test_resting_and_best_price();
    test_crossing_full_fill();
    test_partial_fill_and_price_time_priority();
    test_market_order_sweeps_multiple_levels();
    test_market_order_ioc_drops_remainder();
    test_cancel_is_o1_and_removes_liquidity();
    test_stale_handle_after_slot_reuse_is_rejected();
    test_amend_quantity_decrease_keeps_priority();
    test_amend_price_change_reprioritizes();
    test_pool_exhaustion_is_handled_gracefully();

    if (g_failures == 0) {
        std::printf("\nAll tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
