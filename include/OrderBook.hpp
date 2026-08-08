// OrderBook.hpp
// Limit order book with strict price-time priority. See README.md for the
// full design rationale; the short version:
//
//   - Price levels are a flat, directly-indexed array (tick -> level), not
//     std::map. No pointer chasing, no rebalancing, O(1) level lookup.
//   - Best-bid / best-ask discovery is O(1) via a two-level summary bitmap
//     (see SummaryBitmap below), not a scan and not a heap.
//   - Orders resting within a price level form an intrusive doubly linked
//     list threaded through indices into a single MemoryPool<Order> slab,
//     giving O(1) FIFO insert-at-tail, pop-from-head, and remove-from-middle
//     (cancel) with zero allocation.
//   - Order handles embed a generation counter (Types.hpp) so a stale
//     OrderId can never be used to mutate a slot that's been recycled.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

#include "MemoryPool.hpp"
#include "Types.hpp"

namespace hft {

// ---------------------------------------------------------------------------
// SummaryBitmap<NumBits>
// ---------------------------------------------------------------------------
// A two-level bitmap over `NumBits` price levels giving true O(1)
// (branch-predictable, ~2-3 instructions) "find lowest set bit" / "find
// highest set bit" -- i.e. O(1) best-ask / best-bid discovery no matter how
// many price levels are empty in between.
//
// Level 0 ("bits_"): one bit per price level, packed into 64-bit words.
// Level 1 ("summary_"): one bit per level-0 *word*, telling us in a single
// load whether that whole 64-level block has anything set at all.
//
// This only works as genuine O(1) because level 1 is sized to fit in a
// single machine word (<=64 level-0 words = <=4096 price levels covered).
// That bound is a deliberate scope decision for this project, not an
// oversight: it's the same trick real venues use over a bounded tick range
// per instrument, and it's called out in the README with the extension path
// (a 3-level bitmap) for wider ranges.
template <std::size_t NumBits>
class SummaryBitmap {
    static_assert(NumBits % 64 == 0, "NumBits must be a multiple of 64");
    static constexpr std::size_t kWords = NumBits / 64;
    static_assert(kWords <= 64,
                  "Level-1 summary must fit in one uint64_t for O(1) lookup; "
                  "widen to a 3-level bitmap if you need more than 4096 levels");

public:
    void set(std::uint32_t idx) noexcept {
        const std::uint32_t word = idx >> 6;
        bits_[word] |= (std::uint64_t{1} << (idx & 63));
        summary_ |= (std::uint64_t{1} << word);
    }

    void clear(std::uint32_t idx) noexcept {
        const std::uint32_t word = idx >> 6;
        bits_[word] &= ~(std::uint64_t{1} << (idx & 63));
        if (bits_[word] == 0) {
            summary_ &= ~(std::uint64_t{1} << word);
        }
    }

    [[nodiscard]] bool test(std::uint32_t idx) const noexcept {
        return (bits_[idx >> 6] & (std::uint64_t{1} << (idx & 63))) != 0;
    }

    // Lowest set bit overall, or kNullIndex if the bitmap is empty.
    [[nodiscard]] std::uint32_t find_lowest() const noexcept {
        if (summary_ == 0) return kNullIndex;
        const std::uint32_t word = static_cast<std::uint32_t>(std::countr_zero(summary_));
        const std::uint32_t bit  = static_cast<std::uint32_t>(std::countr_zero(bits_[word]));
        return word * 64 + bit;
    }

    // Highest set bit overall, or kNullIndex if the bitmap is empty.
    [[nodiscard]] std::uint32_t find_highest() const noexcept {
        if (summary_ == 0) return kNullIndex;
        const std::uint32_t word = 63 - static_cast<std::uint32_t>(std::countl_zero(summary_));
        const std::uint32_t bit  = 63 - static_cast<std::uint32_t>(std::countl_zero(bits_[word]));
        return word * 64 + bit;
    }

    [[nodiscard]] bool empty() const noexcept { return summary_ == 0; }

private:
    std::array<std::uint64_t, kWords> bits_{};
    std::uint64_t summary_ = 0;
};

// ---------------------------------------------------------------------------
// Order: the intrusive linked-list node held in the memory pool.
// ---------------------------------------------------------------------------
// Deliberately padded to exactly one cache line (64 bytes) and aligned to
// 64, so that a) it never straddles two cache lines and b) two adjacent
// pool slots accessed by different logical operations never false-share.
struct alignas(64) Order {
    OrderId       order_id   = 0;           // generation<<32 | slot, see Types.hpp
    Price         price      = 0;
    Quantity      quantity   = 0;           // remaining (unfilled) quantity
    std::uint32_t prev       = kNullIndex;  // intrusive doubly-linked list (pool slot indices)
    std::uint32_t next       = kNullIndex;
    Side          side       = Side::Buy;
    bool          active     = false;

    static constexpr std::size_t kUsed =
        sizeof(order_id) + sizeof(price) + sizeof(quantity) + sizeof(prev) +
        sizeof(next) + sizeof(side) + sizeof(active);
    std::array<std::byte, 64 - kUsed> _pad{};
};
static_assert(sizeof(Order) == 64, "Order must occupy exactly one cache line");

// One resting price level: FIFO queue (head=oldest/front-of-queue,
// tail=newest) of orders at that exact price, plus a running total so
// depth-of-book queries don't need to walk the list.
struct PriceLevel {
    std::uint32_t head      = kNullIndex;
    std::uint32_t tail      = kNullIndex;
    std::uint64_t total_qty = 0;
};

// ---------------------------------------------------------------------------
// OrderBook
// ---------------------------------------------------------------------------
template <std::size_t MaxOrders = 1u << 20, std::size_t NumPriceLevels = 4096>
class OrderBook {
    static_assert(NumPriceLevels % 64 == 0 && NumPriceLevels <= 4096);

public:
    OrderBook() noexcept {
        // Generations start at 1, not 0: slot 0 + generation 0 would encode
        // to OrderId 0, which collides with the "no resting order" sentinel
        // used by ExecReport::order_id (0 == fully filled / rejected).
        generation_.fill(1);
    }

    // Feeds one incoming order (limit or market) into the matching engine.
    // Strict price-time priority: crosses the book first (best price, then
    // oldest order at that price), then rests any unfilled remainder (limit
    // orders only -- market orders are IOC and simply drop the remainder).
    // O(1) amortized: matching cost is proportional to the number of resting
    // orders actually consumed, which is intrinsic to the operation, not to
    // any bookkeeping overhead.
    ExecReport match_or_add(const IncomingOrder& incoming) noexcept {
        ExecReport report{.order_id = 0, .filled_qty = 0, .remaining_qty = incoming.quantity, .accepted = true};

        if (incoming.side == Side::Buy) {
            match_against(asks_, ask_bitmap_, incoming.price, incoming.type, report);
        } else {
            match_against(bids_, bid_bitmap_, incoming.price, incoming.type, report);
        }

        if (report.remaining_qty > 0 && incoming.type == OrderType::Limit) {
            const std::uint32_t slot = pool_.acquire();
            if (slot == kNullIndex) [[unlikely]] {
                // Pool exhausted: reject rather than silently drop context.
                report.accepted = (report.filled_qty > 0); // partial fill still "happened"
                return report;
            }

            Order& node   = pool_[slot];
            node.order_id = OrderHandle{.slot = slot, .generation = generation_[slot]}.encode();
            node.price    = incoming.price;
            node.quantity = report.remaining_qty;
            node.side     = incoming.side;
            node.active   = true;
            node.prev     = kNullIndex;
            node.next     = kNullIndex;

            const std::uint32_t level_idx = price_to_index(incoming.price);
            if (incoming.side == Side::Buy) {
                append(bids_[level_idx], bid_bitmap_, level_idx, slot, node.quantity);
            } else {
                append(asks_[level_idx], ask_bitmap_, level_idx, slot, node.quantity);
            }

            report.order_id = node.order_id;
        }

        return report;
    }

    // O(1): decode the handle, validate its generation against the live
    // slot (rejects stale/double cancels), unlink from the intrusive list,
    // update the level's aggregate quantity / bitmap, and recycle the slot.
    bool cancel_order(OrderId id) noexcept {
        const OrderHandle h = OrderHandle::decode(id);
        if (h.slot >= MaxOrders || generation_[h.slot] != h.generation) {
            return false; // stale handle or out-of-range: no-op, not a crash
        }
        Order& node = pool_[h.slot];
        if (!node.active) {
            return false;
        }

        const std::uint32_t level_idx = price_to_index(node.price);
        if (node.side == Side::Buy) {
            unlink(bids_[level_idx], bid_bitmap_, level_idx, h.slot);
        } else {
            unlink(asks_[level_idx], ask_bitmap_, level_idx, h.slot);
        }

        node.active = false;
        ++generation_[h.slot];
        pool_.release(h.slot);
        return true;
    }

    // Amend semantics mirror real exchanges: a quantity-only *decrease* at
    // the same price keeps the order's place in the time-priority queue
    // (in-place mutation). Any price change, or a quantity *increase*, is
    // treated as cancel-replace and loses time priority -- this matches how
    // venues like Nasdaq/CME actually behave and is a deliberate choice, not
    // a simplification.
    ExecReport amend_order(OrderId id, Price new_price, Quantity new_quantity) noexcept {
        const OrderHandle h = OrderHandle::decode(id);
        if (h.slot >= MaxOrders || generation_[h.slot] != h.generation || !pool_[h.slot].active) {
            return ExecReport{.order_id = 0, .filled_qty = 0, .remaining_qty = 0, .accepted = false};
        }

        Order& node = pool_[h.slot];
        const bool price_unchanged = (node.price == new_price);
        const bool qty_decreased   = (new_quantity <= node.quantity);

        if (price_unchanged && qty_decreased) {
            const std::uint32_t level_idx = price_to_index(node.price);
            PriceLevel& level = (node.side == Side::Buy) ? bids_[level_idx] : asks_[level_idx];
            level.total_qty -= (node.quantity - new_quantity);
            node.quantity = new_quantity;
            return ExecReport{.order_id = id, .filled_qty = 0, .remaining_qty = new_quantity, .accepted = true};
        }

        // Cancel-replace path: re-enter as a brand new order (fresh handle,
        // fresh queue position), which also lets it immediately cross the
        // book if the new price is marketable.
        cancel_order(id);
        return match_or_add(IncomingOrder{.price = new_price, .quantity = new_quantity, .side = node.side, .type = OrderType::Limit});
    }

    [[nodiscard]] bool best_bid(Price& out_price, Quantity& out_qty) const noexcept {
        return best_of(bids_, bid_bitmap_, /*want_highest=*/true, out_price, out_qty);
    }
    [[nodiscard]] bool best_ask(Price& out_price, Quantity& out_qty) const noexcept {
        return best_of(asks_, ask_bitmap_, /*want_highest=*/false, out_price, out_qty);
    }

    [[nodiscard]] std::size_t orders_in_use() const noexcept { return pool_.in_use(); }
    [[nodiscard]] std::size_t pool_capacity() const noexcept { return pool_.capacity(); }

private:
    using LevelArray  = std::array<PriceLevel, NumPriceLevels>;
    using BitmapType  = SummaryBitmap<NumPriceLevels>;

    // Maps a raw price tick to a slot in the fixed-size level array. Prices
    // outside [0, NumPriceLevels) are clamped rather than trapped, so a
    // pathological/fat-fingered input degrades to "marketable against the
    // book edge" instead of crashing the engine -- a deliberate robustness
    // choice for a system that must never throw on the hot path.
    [[nodiscard]] static constexpr std::uint32_t price_to_index(Price p) noexcept {
        return p >= NumPriceLevels ? static_cast<std::uint32_t>(NumPriceLevels - 1) : p;
    }

    void append(PriceLevel& level, BitmapType& bitmap, std::uint32_t level_idx,
                std::uint32_t slot, Quantity qty) noexcept {
        Order& node = pool_[slot];
        if (level.head == kNullIndex) {
            level.head = slot;
            bitmap.set(level_idx);
        } else {
            pool_[level.tail].next = slot;
        }
        node.prev   = level.tail;
        node.next   = kNullIndex;
        level.tail  = slot;
        level.total_qty += qty;
    }

    void unlink(PriceLevel& level, BitmapType& bitmap, std::uint32_t level_idx,
                std::uint32_t slot) noexcept {
        Order& node = pool_[slot];
        if (node.prev != kNullIndex) pool_[node.prev].next = node.next;
        else                         level.head = node.next;
        if (node.next != kNullIndex) pool_[node.next].prev = node.prev;
        else                         level.tail = node.prev;

        level.total_qty -= node.quantity;
        if (level.head == kNullIndex) {
            bitmap.clear(level_idx);
        }
    }

    // Sweeps `book`/`bitmap` (the counter-side book) against an incoming
    // order, filling `report` in place. `limit_price` is ignored for Market
    // orders (matches at whatever the book offers).
    void match_against(LevelArray& book, BitmapType& bitmap, Price limit_price,
                        OrderType type, ExecReport& report) noexcept {
        // Determine, from which array we were handed, whether the incoming
        // side is Buy (crossing asks_, ascending) or Sell (crossing bids_,
        // descending). We infer it structurally instead of passing an extra
        // flag: asks_ is only ever passed when the incoming side is Buy.
        const bool crossing_asks = (&book == &asks_);

        while (report.remaining_qty > 0) {
            const std::uint32_t level_idx = crossing_asks ? bitmap.find_lowest() : bitmap.find_highest();
            if (level_idx == kNullIndex) break; // counter-book empty

            if (type == OrderType::Limit) {
                const bool marketable = crossing_asks ? (limit_price >= level_idx)
                                                        : (limit_price <= level_idx);
                if (!marketable) break;
            }

            PriceLevel& level = book[level_idx];
            while (report.remaining_qty > 0 && level.head != kNullIndex) {
                const std::uint32_t resting_slot = level.head;
                Order& resting = pool_[resting_slot];

                const Quantity traded = std::min(report.remaining_qty, resting.quantity);
                resting.quantity      -= traded;
                level.total_qty       -= traded;
                report.remaining_qty  -= traded;
                report.filled_qty     += traded;

                if (resting.quantity == 0) {
                    // Fully filled: pop from the front of the FIFO queue.
                    level.head = resting.next;
                    if (level.head == kNullIndex) level.tail = kNullIndex;
                    else pool_[level.head].prev = kNullIndex;

                    resting.active = false;
                    ++generation_[resting_slot];
                    pool_.release(resting_slot);
                }
            }
            if (level.head == kNullIndex) {
                bitmap.clear(level_idx);
            }
        }
    }

    [[nodiscard]] bool best_of(const LevelArray& book, const BitmapType& bitmap, bool want_highest,
                                Price& out_price, Quantity& out_qty) const noexcept {
        const std::uint32_t idx = want_highest ? bitmap.find_highest() : bitmap.find_lowest();
        if (idx == kNullIndex) return false;
        out_price = static_cast<Price>(idx);
        out_qty   = static_cast<Quantity>(book[idx].total_qty);
        return true;
    }

    MemoryPool<Order, MaxOrders> pool_;

    LevelArray bids_{};
    LevelArray asks_{};
    BitmapType bid_bitmap_{};
    BitmapType ask_bitmap_{};

    // Generation counter per pool slot; incremented every time a slot is
    // released so stale OrderHandles are rejected in O(1). See Types.hpp.
    std::array<std::uint32_t, MaxOrders> generation_{};
};

} // namespace hft
