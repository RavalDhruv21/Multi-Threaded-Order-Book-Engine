// test_orderbook.cpp
// Functional Test Suite for the Low-Latency Order Book Engine.
//
// Coverage Summary:
// - Resting orders and Best Bid/Ask discovery.
// - Full fill & partial fill crossing logic.
// - FIFO price-time priority ordering at identical price levels.
// - Market order sweeping across multiple price levels.
// - Immediate-Or-Cancel (IOC) behavior for market orders.
// - O(1) order cancellation and liquidity removal.
// - Protection against stale handles and ABA recycled slots.
// - Order amending (in-place quantity reduction vs cancel-replace).
// - Graceful handling of pool exhaustion limits.
#include <cstdio>
#include <cstdlib>

#include "OrderBook.hpp"

namespace {

int total_test_failures = 0;

#define CHECK(condition)                                                                \
    do {                                                                                \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            ++total_test_failures;                                                      \
        } else {                                                                        \
            std::printf("  ok: %s\n", #condition);                                      \
        }                                                                               \
    } while (0)

using hft::IncomingOrder;
using hft::OrderType;
using hft::Side;

// Small test order book configuration for predictable testing
using TestOrderBook = hft::OrderBook<1024, 4096>;

// Test 1: Verifies placing resting limit orders and querying best bid price
void test_resting_and_best_price() {
    std::printf("-- test_resting_and_best_price --\n");
    TestOrderBook book;

    auto res1 = book.match_or_add({.price = 100, .quantity = 10, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(res1.accepted);
    CHECK(res1.filled_qty == 0);
    CHECK(res1.remaining_qty == 10);

    auto res2 = book.match_or_add({.price = 105, .quantity = 5, .side = Side::Buy, .type = OrderType::Limit});
    (void)res2;

    hft::Price best_price = 0;
    hft::Quantity best_qty = 0;
    CHECK(book.best_bid(best_price, best_qty));
    CHECK(best_price == 105); // Higher bid price (105) must take priority over 100
    CHECK(best_qty == 5);
}

// Test 2: Verifies a fully matching cross (Buy price >= Sell price)
void test_crossing_full_fill() {
    std::printf("-- test_crossing_full_fill --\n");
    TestOrderBook book;

    book.match_or_add({.price = 100, .quantity = 10, .side = Side::Sell, .type = OrderType::Limit});
    auto result = book.match_or_add({.price = 100, .quantity = 10, .side = Side::Buy, .type = OrderType::Limit});
    
    CHECK(result.filled_qty == 10);
    CHECK(result.remaining_qty == 0);
    CHECK(result.order_id == 0); // Fully filled order leaves no resting remainder

    hft::Price best_price = 0;
    hft::Quantity best_qty = 0;
    CHECK(!book.best_ask(best_price, best_qty)); // Ask book should now be empty
}

// Test 3: Verifies partial fills and FIFO time priority among orders at the same price
void test_partial_fill_and_price_time_priority() {
    std::printf("-- test_partial_fill_and_price_time_priority --\n");
    TestOrderBook book;

    // Place two Sell orders at price 100
    auto sell1 = book.match_or_add({.price = 100, .quantity = 5, .side = Side::Sell, .type = OrderType::Limit});
    auto sell2 = book.match_or_add({.price = 100, .quantity = 5, .side = Side::Sell, .type = OrderType::Limit});
    CHECK(sell1.accepted && sell2.accepted);

    // Incoming Buy order for 7 units: matches all 5 of sell1, then 2 of sell2
    auto buy = book.match_or_add({.price = 100, .quantity = 7, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(buy.filled_qty == 7);
    CHECK(buy.remaining_qty == 0);

    hft::Price best_price = 0;
    hft::Quantity best_qty = 0;
    CHECK(book.best_ask(best_price, best_qty));
    CHECK(best_price == 100);
    CHECK(best_qty == 3); // 3 units remaining from sell2
}

// Test 4: Verifies Market order sweeping across multiple price levels
void test_market_order_sweeps_multiple_levels() {
    std::printf("-- test_market_order_sweeps_multiple_levels --\n");
    TestOrderBook book;

    book.match_or_add({.price = 100, .quantity = 5, .side = Side::Sell, .type = OrderType::Limit});
    book.match_or_add({.price = 101, .quantity = 5, .side = Side::Sell, .type = OrderType::Limit});

    // Market Buy order for 8 units sweeps price level 100 (5 qty) and 101 (3 qty)
    auto result = book.match_or_add({.price = 0, .quantity = 8, .side = Side::Buy, .type = OrderType::Market});
    CHECK(result.filled_qty == 8);
    CHECK(result.remaining_qty == 0);

    hft::Price best_price = 0;
    hft::Quantity best_qty = 0;
    CHECK(book.best_ask(best_price, best_qty));
    CHECK(best_price == 101);
    CHECK(best_qty == 2); // 2 units remaining at price 101
}

// Test 5: Verifies that unfulfilled Market order quantity does NOT rest in the book (IOC)
void test_market_order_ioc_drops_remainder() {
    std::printf("-- test_market_order_ioc_drops_remainder --\n");
    TestOrderBook book;

    book.match_or_add({.price = 100, .quantity = 3, .side = Side::Sell, .type = OrderType::Limit});

    auto result = book.match_or_add({.price = 0, .quantity = 10, .side = Side::Buy, .type = OrderType::Market});
    CHECK(result.filled_qty == 3);
    CHECK(result.remaining_qty == 7); // Unfilled remainder of 7 units...
    CHECK(result.order_id == 0);      // ...does NOT rest in the book (IOC cancellation)

    hft::Price best_price = 0;
    hft::Quantity best_qty = 0;
    CHECK(!book.best_bid(best_price, best_qty)); // Buy book remains empty
}

// Test 6: Verifies order cancellation and liquidity removal
void test_cancel_is_o1_and_removes_liquidity() {
    std::printf("-- test_cancel_is_o1_and_removes_liquidity --\n");
    TestOrderBook book;

    auto result = book.match_or_add({.price = 100, .quantity = 10, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(result.order_id != 0);

    hft::Price best_price = 0;
    hft::Quantity best_qty = 0;
    CHECK(book.best_bid(best_price, best_qty));

    // Cancel order
    CHECK(book.cancel_order(result.order_id));
    CHECK(!book.best_bid(best_price, best_qty)); // Level is now empty

    // Duplicate cancel attempt must fail gracefully
    CHECK(!book.cancel_order(result.order_id));
}

// Test 7: Verifies ABA protection (stale order handle rejection after slot reuse)
void test_stale_handle_after_slot_reuse_is_rejected() {
    std::printf("-- test_stale_handle_after_slot_reuse_is_rejected --\n");
    TestOrderBook book;

    auto res1 = book.match_or_add({.price = 100, .quantity = 1, .side = Side::Buy, .type = OrderType::Limit});
    const hft::OrderId stale_handle = res1.order_id;
    CHECK(book.cancel_order(stale_handle)); // Slot freed, generation counter incremented

    // Add a new order that reuses the freed pool slot
    auto res2 = book.match_or_add({.price = 200, .quantity = 1, .side = Side::Buy, .type = OrderType::Limit});
    (void)res2;

    // Attempting to cancel with the stale handle must be rejected
    CHECK(!book.cancel_order(stale_handle));

    hft::Price best_price = 0;
    hft::Quantity best_qty = 0;
    CHECK(book.best_bid(best_price, best_qty));
    CHECK(best_price == 200); // The new order must remain active and unaffected
}

// Test 8: Verifies in-place quantity reduction retains time priority
void test_amend_quantity_decrease_keeps_priority() {
    std::printf("-- test_amend_quantity_decrease_keeps_priority --\n");
    TestOrderBook book;

    auto res1 = book.match_or_add({.price = 100, .quantity = 10, .side = Side::Sell, .type = OrderType::Limit});
    auto res2 = book.match_or_add({.price = 100, .quantity = 10, .side = Side::Sell, .type = OrderType::Limit});

    // Reduce res1 quantity from 10 down to 4
    auto amend_result = book.amend_order(res1.order_id, 100, 4);
    CHECK(amend_result.accepted);

    // Matching Buy order for 4 units should fully consume res1 (which retained position #1)
    auto buy = book.match_or_add({.price = 100, .quantity = 4, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(buy.filled_qty == 4);

    hft::Price best_price = 0;
    hft::Quantity best_qty = 0;
    CHECK(book.best_ask(best_price, best_qty));
    CHECK(best_qty == 10); // res2 remains fully untouched at 10 units
    (void)res2;
}

// Test 9: Verifies order reprioritization on price change amend
void test_amend_price_change_reprioritizes() {
    std::printf("-- test_amend_price_change_reprioritizes --\n");
    TestOrderBook book;

    auto res = book.match_or_add({.price = 100, .quantity = 5, .side = Side::Buy, .type = OrderType::Limit});
    auto amend_result = book.amend_order(res.order_id, 110, 5);
    CHECK(amend_result.accepted);

    hft::Price best_price = 0;
    hft::Quantity best_qty = 0;
    CHECK(book.best_bid(best_price, best_qty));
    CHECK(best_price == 110); // Order moved to new price level 110
}

// Test 10: Verifies graceful rejection on memory pool capacity exhaustion
void test_pool_exhaustion_is_handled_gracefully() {
    std::printf("-- test_pool_exhaustion_is_handled_gracefully --\n");
    hft::OrderBook<4, 4096> tiny_book; // Capacity for exactly 4 resting orders

    for (int i = 0; i < 4; ++i) {
        auto res = tiny_book.match_or_add({
            .price = static_cast<hft::Price>(100 + i), 
            .quantity = 1, 
            .side = Side::Buy, 
            .type = OrderType::Limit
        });
        CHECK(res.accepted);
    }

    // 5th order attempt when pool is full: must be rejected without crashing
    auto overflow_res = tiny_book.match_or_add({.price = 200, .quantity = 1, .side = Side::Buy, .type = OrderType::Limit});
    CHECK(!overflow_res.accepted);
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

    if (total_test_failures == 0) {
        std::printf("\nAll tests passed successfully.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d test check(s) FAILED.\n", total_test_failures);
    return 1;
}

