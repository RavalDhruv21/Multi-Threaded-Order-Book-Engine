// Types.hpp
// Clean, readable domain types, order structures, and standard container
// representations for a C++ Order Book matching engine.
//
// Designed for clarity and ease of understanding by intermediate programmers.
#pragma once

#include <cstdint>
#include <limits>
#include <vector>
#include <map>
#include <list>
#include <string>

namespace hft {

// ---------------------------------------------------------------------------
// Basic Domain Primitives
// ---------------------------------------------------------------------------

// Prices are represented as integer ticks rather than floating-point numbers.
// In financial systems, floating-point math causes precision issues (e.g.,
// 0.1 + 0.2 != 0.3). Using integer ticks guarantees exact arithmetic.
using Price = std::uint32_t;

// Quantity represents the number of shares or contracts in an order.
using Quantity = std::uint32_t;

// OrderId is a unique 64-bit unsigned integer assigned to each resting order.
using OrderId = std::uint64_t;

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

// Defines whether an order is buying or selling.
// - Buy: Bidding to purchase shares (Bid side)
// - Sell: Offering to sell shares (Ask side)
enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1
};

// Defines the execution policy of an order.
// - Limit: Specifies a maximum purchase price or minimum sale price.
// - Market: Executes immediately at the best available market price.
enum class OrderType : std::uint8_t {
    Limit = 0,
    Market = 1
};

// Sentinel value used to represent an invalid or unassigned index/ID.
constexpr std::uint32_t kNullIndex = std::numeric_limits<std::uint32_t>::max();

// ---------------------------------------------------------------------------
// Order Handle (Slot & Generation Tracking)
// ---------------------------------------------------------------------------
// To uniquely identify resting orders without raw pointers, an OrderId packs:
//   1. Memory pool slot index (lower 32 bits)
//   2. Generation counter (upper 32 bits)
//
// Bumping the generation counter whenever an order slot is released prevents the
// "ABA problem" where a stale handle accidentally matches a new order in the same slot.
struct OrderHandle {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;

    // Combines slot index and generation counter into a single 64-bit OrderId.
    [[nodiscard]] constexpr OrderId encode() const noexcept {
        const auto gen_64 = static_cast<std::uint64_t>(generation);
        const auto slot_64 = static_cast<std::uint64_t>(slot);
        return (gen_64 << 32) | slot_64;
    }

    // Extracts the slot index and generation counter from a 64-bit OrderId.
    static constexpr OrderHandle decode(OrderId id) noexcept {
        const auto slot_part = static_cast<std::uint32_t>(id & 0xFFFFFFFFu);
        const auto gen_part  = static_cast<std::uint32_t>(id >> 32);
        return OrderHandle{
            .slot = slot_part,
            .generation = gen_part
        };
    }
};

// ---------------------------------------------------------------------------
// Incoming Order Message
// ---------------------------------------------------------------------------
// Represents an order submitted to the matching engine.
struct IncomingOrder {
    Price     price = 0;      // Price limit (ignored for Market orders)
    Quantity  quantity = 0;   // Number of units requested
    Side      side = Side::Buy;
    OrderType type = OrderType::Limit;
};

// ---------------------------------------------------------------------------
// Execution Report Message
// ---------------------------------------------------------------------------
// Returns the result of matching or placing an order in the book.
struct ExecReport {
    OrderId  order_id = 0;      // Assigned ID if remainder rests in book (0 if fully filled/rejected)
    Quantity filled_qty = 0;    // Number of units matched and executed
    Quantity remaining_qty = 0; // Number of units remaining unfilled
    bool     accepted = false;  // True if order was successfully processed
};

// ---------------------------------------------------------------------------
// Core Order Representation (Standard C++ Structures)
// ---------------------------------------------------------------------------
// Represents an active order resting in the order book.
struct Order {
    OrderId       order_id = 0;      // Unique order identifier
    Price         price = 0;         // Limit price tick
    Quantity      quantity = 0;      // Remaining unfilled quantity
    std::uint32_t prev = kNullIndex; // Linked list index (previous order at same price)
    std::uint32_t next = kNullIndex; // Linked list index (next order at same price)
    Side          side = Side::Buy;  // Buy (Bid) or Sell (Ask)
    bool          active = false;    // Whether this order is currently active in the book
};

// ---------------------------------------------------------------------------
// Standard Container Definitions for Order Book Storage
// ---------------------------------------------------------------------------
// Standard containers provide clear, easy-to-read abstractions for the Order Book:
//
// 1. Price-Time Priority (FIFO Queue):
//    Orders at the exact same price level are kept in a FIFO list (std::list<Order>).
//    The earliest order (head of list) is matched first.
using OrderQueue = std::list<Order>;

// 2. Bid Book (Buy Orders):
//    Bids are ordered by price in DESCENDING order (highest bid price first).
//    std::map sorted with std::greater<Price> provides O(log N) price access
//    and automatically keeps the best bid at the top.
using BidBook = std::map<Price, OrderQueue, std::greater<Price>>;

// 3. Ask Book (Sell Orders):
//    Asks are ordered by price in ASCENDING order (lowest ask price first).
//    std::map sorted with std::less<Price> provides O(log N) price access
//    and automatically keeps the best ask at the top.
using AskBook = std::map<Price, OrderQueue, std::less<Price>>;

// ---------------------------------------------------------------------------
// Order Matching Logic Concept & Rules (Contextual Explanation)
// ---------------------------------------------------------------------------
//
// How Bid / Ask Cross-Matching Works:
//
// 1. Incoming BUY Limit Order (Price P):
//    - Matches against resting SELL orders (Asks) with Price <= P.
//    - Sweeps lowest ask prices first.
//    - At each price level, matches orders in FIFO order (oldest order first).
//    - Any unfilled remaining quantity rests on the Bid side at price P.
//
// 2. Incoming SELL Limit Order (Price P):
//    - Matches against resting BUY orders (Bids) with Price >= P.
//    - Sweeps highest bid prices first.
//    - At each price level, matches orders in FIFO order (oldest order first).
//    - Any unfilled remaining quantity rests on the Ask side at price P.
//
// 3. Market Orders:
//    - Execute immediately against the best available opposite orders regardless of price.
//    - Any unfilled remaining quantity is cancelled (does NOT rest in the book).
// ---------------------------------------------------------------------------

} // namespace hft

