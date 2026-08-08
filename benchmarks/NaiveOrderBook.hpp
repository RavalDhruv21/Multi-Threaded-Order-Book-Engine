// NaiveOrderBook.hpp
// The "obvious first draft" implementation most engineers reach for:
// std::map<Price, std::list<Order>> for price levels plus a
// std::unordered_map<OrderId, iterator> for O(1)-ish cancel lookup.
//
// This is NOT part of the engine (include/OrderBook.hpp) -- it exists
// purely as a measurement baseline so the design decisions in OrderBook.hpp
// (flat array + summary bitmap + intrusive lists + memory pool) can be
// backed by numbers instead of assertions. See README.md's "Measured
// Performance" section for the perf stat comparison this produces.
//
// Every allocation here is a real heap allocation: std::map does a
// red-black-tree node alloc per *first order at a new price level*, and
// std::list does a node alloc per *order*. That's exactly the pointer-chasing,
// cache-hostile behavior the pooled/intrusive design in OrderBook.hpp is
// built to avoid.
#pragma once

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>

#include "../include/Types.hpp"

namespace hft::naive {

struct NaiveOrder {
    OrderId  order_id;
    Price    price;
    Quantity quantity;
    Side     side;
};

class NaiveOrderBook {
public:
    ExecReport match_or_add(const IncomingOrder& incoming) {
        ExecReport report{.order_id = 0, .filled_qty = 0, .remaining_qty = incoming.quantity, .accepted = true};

        if (incoming.side == Side::Buy) {
            match_against(asks_, incoming.price, incoming.type, report, /*ascending=*/true);
        } else {
            match_against(bids_, incoming.price, incoming.type, report, /*ascending=*/false);
        }

        if (report.remaining_qty > 0 && incoming.type == OrderType::Limit) {
            const OrderId id = next_id_++;
            auto& level = (incoming.side == Side::Buy) ? bids_[incoming.price] : asks_[incoming.price];
            level.push_back(NaiveOrder{id, incoming.price, report.remaining_qty, incoming.side});
            index_[id] = IndexEntry{incoming.side, incoming.price, std::prev(level.end())};
            report.order_id = id;
        }

        return report;
    }

    bool cancel_order(OrderId id) {
        auto found = index_.find(id);
        if (found == index_.end()) return false;

        auto& entry = found->second;
        auto& book  = (entry.side == Side::Buy) ? bids_ : asks_;
        auto level_it = book.find(entry.price);
        level_it->second.erase(entry.it);
        if (level_it->second.empty()) book.erase(level_it);

        index_.erase(found);
        return true;
    }

    [[nodiscard]] bool best_bid(Price& p, Quantity& q) const { return best_of(bids_, p, q); }
    [[nodiscard]] bool best_ask(Price& p, Quantity& q) const { return best_of(asks_, p, q); }

private:
    using Levels = std::map<Price, std::list<NaiveOrder>>;

    template <typename Book>
    void match_against(Book& book, Price limit_price, OrderType type, ExecReport& report, bool ascending) {
        while (report.remaining_qty > 0 && !book.empty()) {
            auto level_it = ascending ? book.begin() : std::prev(book.end());
            const Price level_price = level_it->first;

            if (type == OrderType::Limit) {
                const bool marketable = ascending ? (limit_price >= level_price) : (limit_price <= level_price);
                if (!marketable) break;
            }

            auto& orders = level_it->second;
            while (report.remaining_qty > 0 && !orders.empty()) {
                auto& resting = orders.front();
                const Quantity traded = std::min(report.remaining_qty, resting.quantity);
                resting.quantity     -= traded;
                report.remaining_qty -= traded;
                report.filled_qty    += traded;

                if (resting.quantity == 0) {
                    index_.erase(resting.order_id);
                    orders.pop_front();
                }
            }
            if (orders.empty()) book.erase(level_it);
        }
    }

    [[nodiscard]] bool best_of(const Levels& book, Price& p, Quantity& q) const {
        if (book.empty()) return false;
        // bids_ wants the highest key, asks_ wants the lowest; callers pass
        // the right map and we always report book.begin() for asks-style
        // ascending maps. Since both maps use the same default (ascending)
        // comparator here, "best" for bids_ is the *last* element.
        const auto& [price, orders] = (&book == &bids_) ? *std::prev(book.end()) : *book.begin();
        p = price;
        q = 0;
        for (const auto& o : orders) q += o.quantity;
        return true;
    }

    Levels bids_;
    Levels asks_;
    struct IndexEntry {
        Side side;
        Price price;
        std::list<NaiveOrder>::iterator it;
    };
    std::unordered_map<OrderId, IndexEntry> index_;
    OrderId next_id_ = 1;
};

} // namespace hft::naive
