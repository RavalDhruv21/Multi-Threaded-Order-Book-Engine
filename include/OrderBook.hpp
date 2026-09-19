// OrderBook.hpp
// Limit Order Book matching engine with strict Price-Time priority (FIFO).
//
// Key Design Overview:
// 1. Price-Time Priority:
//    - Orders are sorted by price first (highest Bid, lowest Ask).
//    - Orders at the same price are matched FIFO (oldest order first).
// 2. Direct-Indexed Price Levels:
//    - Price levels are stored in a flat array (`LevelArray`) indexed by tick price.
// 3. Bitmaps for O(1) Best Price Discovery:
//    - `SummaryBitmap` tracks non-empty price levels to quickly find the best bid/ask.
// 4. Intrusive Doubly-Linked Lists & Memory Pool:
//    - Orders resting at a price level form a doubly-linked list using slot indices.
//    - Memory allocations are pre-allocated in `MemoryPool<Order>`, avoiding heap churn.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#if __cplusplus >= 202002L
#include <bit>
#endif

#include "MemoryPool.hpp"
#include "Types.hpp"

namespace hft {

// ---------------------------------------------------------------------------
// SummaryBitmap<NumBits>
// ---------------------------------------------------------------------------
// Two-level bitmap structure over `NumBits` price levels enabling O(1) discovery
// of the best ask (lowest set bit) and best bid (highest set bit).
//
// - Level 0 (`bits_` array): 64 price levels per `uint64_t` word.
// - Level 1 (`summary_` word): 1 bit per Level-0 word, indicating if any level in that 64-block has orders.
template <std::size_t NumBits>
class SummaryBitmap {
    static_assert(NumBits % 64 == 0, "NumBits must be a multiple of 64.");
    static constexpr std::size_t kWords = NumBits / 64;
    static_assert(kWords <= 64, "SummaryBitmap supports up to 4096 price levels (64 words * 64 bits).");

public:
    // Sets bit for a given price level index (marks price level as non-empty).
    void set(std::uint32_t level_index) noexcept {
        const std::uint32_t word_idx = level_index >> 6; // divide by 64
        const std::uint32_t bit_idx  = level_index & 63;  // modulo 64
        bits_[word_idx] |= (std::uint64_t{1} << bit_idx);
        summary_        |= (std::uint64_t{1} << word_idx);
    }

    // Clears bit for a given price level index (marks price level as empty).
    void clear(std::uint32_t level_index) noexcept {
        const std::uint32_t word_idx = level_index >> 6;
        const std::uint32_t bit_idx  = level_index & 63;
        bits_[word_idx] &= ~(std::uint64_t{1} << bit_idx);
        if (bits_[word_idx] == 0) {
            summary_ &= ~(std::uint64_t{1} << word_idx);
        }
    }

    // Tests if a price level has resting orders.
    [[nodiscard]] bool test(std::uint32_t level_index) const noexcept {
        const std::uint32_t word_idx = level_index >> 6;
        const std::uint32_t bit_idx  = level_index & 63;
        return (bits_[word_idx] & (std::uint64_t{1} << bit_idx)) != 0;
    }

    // Finds the lowest set bit index (Best Ask price), or kNullIndex if empty.
    [[nodiscard]] std::uint32_t find_lowest() const noexcept {
        if (summary_ == 0) return kNullIndex;
#if __cplusplus >= 202002L
        const std::uint32_t word_idx = static_cast<std::uint32_t>(std::countr_zero(summary_));
        const std::uint32_t bit_idx  = static_cast<std::uint32_t>(std::countr_zero(bits_[word_idx]));
#else
        const std::uint32_t word_idx = static_cast<std::uint32_t>(__builtin_ctzll(summary_));
        const std::uint32_t bit_idx  = static_cast<std::uint32_t>(__builtin_ctzll(bits_[word_idx]));
#endif
        return word_idx * 64 + bit_idx;
    }

    // Finds the highest set bit index (Best Bid price), or kNullIndex if empty.
    [[nodiscard]] std::uint32_t find_highest() const noexcept {
        if (summary_ == 0) return kNullIndex;
#if __cplusplus >= 202002L
        const std::uint32_t word_idx = 63 - static_cast<std::uint32_t>(std::countl_zero(summary_));
        const std::uint32_t bit_idx  = 63 - static_cast<std::uint32_t>(std::countl_zero(bits_[word_idx]));
#else
        const std::uint32_t word_idx = 63 - static_cast<std::uint32_t>(__builtin_clzll(summary_));
        const std::uint32_t bit_idx  = 63 - static_cast<std::uint32_t>(__builtin_clzll(bits_[word_idx]));
#endif
        return word_idx * 64 + bit_idx;
    }

    // Checks if bitmap contains any set bits.
    [[nodiscard]] bool empty() const noexcept { return summary_ == 0; }

private:
    std::array<std::uint64_t, kWords> bits_{};
    std::uint64_t summary_ = 0;
};

// ---------------------------------------------------------------------------
// PriceLevel
// ---------------------------------------------------------------------------
// Represents a single price tick level containing a FIFO queue of resting orders.
struct PriceLevel {
    std::uint32_t head      = kNullIndex; // Slot index of oldest order (front of queue)
    std::uint32_t tail      = kNullIndex; // Slot index of newest order (back of queue)
    std::uint64_t total_qty = 0;          // Aggregate total quantity resting at this price level
};

// ---------------------------------------------------------------------------
// OrderBook Engine Class
// ---------------------------------------------------------------------------
template <std::size_t MaxOrders = 1u << 20, std::size_t NumPriceLevels = 4096>
class OrderBook {
    static_assert(NumPriceLevels % 64 == 0 && NumPriceLevels <= 4096,
                  "NumPriceLevels must be a multiple of 64 up to 4096.");

public:
    OrderBook() noexcept {
        // Generation counters start at 1 so slot 0 + gen 0 doesn't collide with OrderId 0
        generation_counters_.fill(1);
    }

    // Main entry point: Matches an incoming limit or market order against the book.
    // Any unfilled remainder of a Limit order is added to the resting order book.
    ExecReport match_or_add(const IncomingOrder& incoming) noexcept {
        ExecReport report{
            .order_id      = 0,
            .filled_qty    = 0,
            .remaining_qty = incoming.quantity,
            .accepted      = true
        };

        // Step 1: Match incoming order against opposite side of the book
        if (incoming.side == Side::Buy) {
            // Incoming Buy order matches against resting Sell orders (asks)
            match_against(ask_levels_, ask_bitmap_, incoming.price, incoming.type, report);
        } else {
            // Incoming Sell order matches against resting Buy orders (bids)
            match_against(bid_levels_, bid_bitmap_, incoming.price, incoming.type, report);
        }

        // Step 2: If unfilled quantity remains and order type is Limit, place order in the book
        if (report.remaining_qty > 0 && incoming.type == OrderType::Limit) {
            const std::uint32_t slot = order_pool_.acquire();
            if (slot == kNullIndex) {
                // Pool exhausted: reject resting remainder
                report.accepted = (report.filled_qty > 0);
                return report;
            }

            // Initialize new resting order node in pool slot
            Order& node   = order_pool_[slot];
            node.order_id = OrderHandle{.slot = slot, .generation = generation_counters_[slot]}.encode();
            node.price    = incoming.price;
            node.quantity = report.remaining_qty;
            node.side     = incoming.side;
            node.active   = true;
            node.prev     = kNullIndex;
            node.next     = kNullIndex;

            // Append order node to the appropriate price level FIFO queue
            const std::uint32_t level_idx = price_to_index(incoming.price);
            if (incoming.side == Side::Buy) {
                append_order_to_level(bid_levels_[level_idx], bid_bitmap_, level_idx, slot, node.quantity);
            } else {
                append_order_to_level(ask_levels_[level_idx], ask_bitmap_, level_idx, slot, node.quantity);
            }

            report.order_id = node.order_id;
        }

        return report;
    }

    // Cancels an active order by OrderId. Returns true if successfully unlinked and released.
    bool cancel_order(OrderId order_id) noexcept {
        const OrderHandle handle = OrderHandle::decode(order_id);
        
        // Validate handle slot and generation counter to prevent stale cancels
        if (handle.slot >= MaxOrders || generation_counters_[handle.slot] != handle.generation) {
            return false; // Stale or invalid handle
        }

        Order& node = order_pool_[handle.slot];
        if (!node.active) {
            return false; // Order already cancelled or fully filled
        }

        // Unlink order from price level FIFO queue
        const std::uint32_t level_idx = price_to_index(node.price);
        if (node.side == Side::Buy) {
            unlink_order_from_level(bid_levels_[level_idx], bid_bitmap_, level_idx, handle.slot);
        } else {
            unlink_order_from_level(ask_levels_[level_idx], ask_bitmap_, level_idx, handle.slot);
        }

        // Deactivate order and recycle memory pool slot with incremented generation
        node.active = false;
        ++generation_counters_[handle.slot];
        order_pool_.release(handle.slot);
        return true;
    }

    // Amends an existing order's price or quantity.
    // - Quantity reduction at same price maintains queue time priority.
    // - Price change or quantity increase triggers cancel-replace (loses time priority).
    ExecReport amend_order(OrderId order_id, Price new_price, Quantity new_quantity) noexcept {
        const OrderHandle handle = OrderHandle::decode(order_id);
        if (handle.slot >= MaxOrders || 
            generation_counters_[handle.slot] != handle.generation || 
            !order_pool_[handle.slot].active) {
            return ExecReport{.order_id = 0, .filled_qty = 0, .remaining_qty = 0, .accepted = false};
        }

        Order& node = order_pool_[handle.slot];
        const bool price_unchanged = (node.price == new_price);
        const bool qty_decreased   = (new_quantity <= node.quantity);

        // In-place quantity reduction path (retains queue position)
        if (price_unchanged && qty_decreased) {
            const std::uint32_t level_idx = price_to_index(node.price);
            PriceLevel& level = (node.side == Side::Buy) ? bid_levels_[level_idx] : ask_levels_[level_idx];
            level.total_qty -= (node.quantity - new_quantity);
            node.quantity = new_quantity;
            return ExecReport{.order_id = order_id, .filled_qty = 0, .remaining_qty = new_quantity, .accepted = true};
        }

        // Cancel-replace path: Cancel old order and submit replacement
        const Side original_side = node.side;
        cancel_order(order_id);
        return match_or_add(IncomingOrder{
            .price = new_price, 
            .quantity = new_quantity, 
            .side = original_side, 
            .type = OrderType::Limit
        });
    }

    // Retrieves current Best Bid price and quantity. Returns false if Bid side is empty.
    [[nodiscard]] bool best_bid(Price& out_price, Quantity& out_qty) const noexcept {
        return get_best_price_level(bid_levels_, bid_bitmap_, /*want_highest=*/true, out_price, out_qty);
    }

    // Retrieves current Best Ask price and quantity. Returns false if Ask side is empty.
    [[nodiscard]] bool best_ask(Price& out_price, Quantity& out_qty) const noexcept {
        return get_best_price_level(ask_levels_, ask_bitmap_, /*want_highest=*/false, out_price, out_qty);
    }

    // Statistics queries
    [[nodiscard]] std::size_t orders_in_use() const noexcept { return order_pool_.in_use(); }
    [[nodiscard]] std::size_t pool_capacity() const noexcept { return order_pool_.capacity(); }

private:
    using LevelArray = std::array<PriceLevel, NumPriceLevels>;
    using BitmapType = SummaryBitmap<NumPriceLevels>;

    // Clamps price ticks to valid array bounds [0, NumPriceLevels - 1]
    [[nodiscard]] static constexpr std::uint32_t price_to_index(Price price) noexcept {
        return price >= NumPriceLevels ? static_cast<std::uint32_t>(NumPriceLevels - 1) : price;
    }

    // Appends an order slot index to the tail of a price level's FIFO queue
    void append_order_to_level(PriceLevel& level, BitmapType& bitmap, std::uint32_t level_idx,
                               std::uint32_t slot_idx, Quantity qty) noexcept {
        Order& node = order_pool_[slot_idx];
        if (level.head == kNullIndex) {
            level.head = slot_idx;
            bitmap.set(level_idx);
        } else {
            order_pool_[level.tail].next = slot_idx;
        }
        node.prev       = level.tail;
        node.next       = kNullIndex;
        level.tail      = slot_idx;
        level.total_qty += qty;
    }

    // Unlinks an order slot index from a price level's FIFO queue
    void unlink_order_from_level(PriceLevel& level, BitmapType& bitmap, std::uint32_t level_idx,
                                 std::uint32_t slot_idx) noexcept {
        Order& node = order_pool_[slot_idx];
        if (node.prev != kNullIndex) {
            order_pool_[node.prev].next = node.next;
        } else {
            level.head = node.next;
        }

        if (node.next != kNullIndex) {
            order_pool_[node.next].prev = node.prev;
        } else {
            level.tail = node.prev;
        }

        level.total_qty -= node.quantity;
        if (level.head == kNullIndex) {
            bitmap.clear(level_idx);
        }
    }

    // Core order matching loop: Sweeps price levels matching incoming quantity against resting orders
    void match_against(LevelArray& target_levels, BitmapType& bitmap, Price limit_price,
                       OrderType type, ExecReport& report) noexcept {
        const bool crossing_asks = (&target_levels == &ask_levels_);

        while (report.remaining_qty > 0) {
            // Find best price level (lowest ask for buy orders, highest bid for sell orders)
            const std::uint32_t level_idx = crossing_asks ? bitmap.find_lowest() : bitmap.find_highest();
            if (level_idx == kNullIndex) break; // Counter book is completely empty

            // Check if limit price crosses market price
            if (type == OrderType::Limit) {
                const bool marketable = crossing_asks ? (limit_price >= level_idx)
                                                       : (limit_price <= level_idx);
                if (!marketable) break; // Limit price does not cross best available resting price
            }

            PriceLevel& level = target_levels[level_idx];

            // Match against orders in FIFO queue at this price level
            while (report.remaining_qty > 0 && level.head != kNullIndex) {
                const std::uint32_t resting_slot = level.head;
                Order& resting_order              = order_pool_[resting_slot];

                const Quantity traded_qty = std::min(report.remaining_qty, resting_order.quantity);
                resting_order.quantity   -= traded_qty;
                level.total_qty          -= traded_qty;
                report.remaining_qty     -= traded_qty;
                report.filled_qty        += traded_qty;

                if (resting_order.quantity == 0) {
                    // Order fully satisfied: pop head order from FIFO queue
                    level.head = resting_order.next;
                    if (level.head == kNullIndex) {
                        level.tail = kNullIndex;
                    } else {
                        order_pool_[level.head].prev = kNullIndex;
                    }

                    resting_order.active = false;
                    ++generation_counters_[resting_slot];
                    order_pool_.release(resting_slot);
                }
            }

            // Clear level bitmap bit if price level is now empty
            if (level.head == kNullIndex) {
                bitmap.clear(level_idx);
            }
        }
    }

    // Helper method to look up best bid/ask price level
    [[nodiscard]] bool get_best_price_level(const LevelArray& target_levels, const BitmapType& bitmap, 
                                           bool want_highest, Price& out_price, Quantity& out_qty) const noexcept {
        const std::uint32_t idx = want_highest ? bitmap.find_highest() : bitmap.find_lowest();
        if (idx == kNullIndex) return false;
        out_price = static_cast<Price>(idx);
        out_qty   = static_cast<Quantity>(target_levels[idx].total_qty);
        return true;
    }

    // Memory pool storing all pre-allocated Order nodes
    MemoryPool<Order, MaxOrders> order_pool_;

    // Array of price levels for Bids and Asks
    LevelArray bid_levels_{};
    LevelArray ask_levels_{};

    // Bitmaps for fast O(1) best bid and best ask discovery
    BitmapType bid_bitmap_{};
    BitmapType ask_bitmap_{};

    // Generation counters per pool slot to detect stale order handles
    std::array<std::uint32_t, MaxOrders> generation_counters_{};
};

} // namespace hft

