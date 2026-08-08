// Types.hpp
// Strongly-typed primitives shared across the engine. Nothing here allocates
// or does anything clever -- it exists purely so that "price" and "quantity"
// can never be silently swapped at a call site (a real bug class in OMS code).
#pragma once

#include <cstdint>
#include <limits>

namespace hft {

// ---------------------------------------------------------------------------
// Domain primitives
// ---------------------------------------------------------------------------

// Prices are integer ticks, not floating point. Floating point prices are a
// classic correctness bug in matching engines (0.1 + 0.2 != 0.3): every real
// exchange represents price as an integer multiple of a tick size.
using Price    = std::uint32_t;
using Quantity = std::uint32_t;
using OrderId  = std::uint64_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };

// Sentinel used throughout the intrusive linked lists / pool free-list to
// mean "no slot" without needing a pointer (and without needing -1 on an
// unsigned type to be misread as a huge valid index).
inline constexpr std::uint32_t kNullIndex = std::numeric_limits<std::uint32_t>::max();

// ---------------------------------------------------------------------------
// Order handle encoding
// ---------------------------------------------------------------------------
// order_id is not a naive monotonic counter. It packs a generation counter
// in the high 32 bits and the memory-pool slot index in the low 32 bits:
//
//   [ 63 .......... 32 | 31 .......... 0 ]
//   [   generation      |   pool slot    ]
//
// Why: pool slots are recycled the instant an order is cancelled or fully
// filled. If IDs were just "the slot index", a stale OrderId held by a client
// after cancellation could accidentally address a *different, newer* order
// that reused the same slot (classic ABA problem). Bumping the generation on
// every release and validating it on lookup turns a silent correctness bug
// into a safe, O(1)-detectable no-op.
struct OrderHandle {
    std::uint32_t slot;
    std::uint32_t generation;

    [[nodiscard]] constexpr OrderId encode() const noexcept {
        return (static_cast<OrderId>(generation) << 32) | slot;
    }

    static constexpr OrderHandle decode(OrderId id) noexcept {
        return OrderHandle{
            .slot       = static_cast<std::uint32_t>(id & 0xFFFF'FFFFu),
            .generation = static_cast<std::uint32_t>(id >> 32),
        };
    }
};

// ---------------------------------------------------------------------------
// Wire-format message that flows through the SPSC queue from the market-data
// / client thread to the matching engine thread. Deliberately tiny (16
// bytes) and trivially copyable so it can be memcpy'd across the ring buffer
// with no allocation and no hidden ownership transfer.
// ---------------------------------------------------------------------------
struct alignas(16) IncomingOrder {
    Price      price;      // ignored for Market orders
    Quantity   quantity;
    Side       side;
    OrderType  type;
};
static_assert(sizeof(IncomingOrder) == 16);
static_assert(std::is_trivially_copyable_v<IncomingOrder>);

// Result of feeding one incoming order into the book. Reused across calls by
// the caller (never heap-allocated by the engine) so it can be filled in and
// inspected without allocation on the hot path.
struct alignas(16) ExecReport {
    OrderId  order_id;      // handle assigned to the resting remainder (0 if fully filled/rejected)
    Quantity filled_qty;
    Quantity remaining_qty;
    bool     accepted;      // false if book/pool was full and the order was dropped
};

} // namespace hft
