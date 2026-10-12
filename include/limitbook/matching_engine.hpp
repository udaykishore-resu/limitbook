#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>

#include "limitbook/order_book.hpp"
#include "limitbook/order_index.hpp"
#include "limitbook/types.hpp"

namespace limitbook {

struct Fill {
    OrderId taker;
    OrderId maker;
    Side taker_side;
    Price price;  // Always the maker's (resting) price.
    Qty qty;

    bool operator==(const Fill&) const = default;
};

// The sink is a template parameter constrained by a concept, not a
// std::function or virtual interface: the call is resolved at compile time,
// inlines, and never allocates. A sink must not re-enter the engine.
template <typename S>
concept FillSink = requires(S& sink, const Fill& fill) {
    { sink.on_fill(fill) } -> std::same_as<void>;
};

enum class TimeInForce : std::uint8_t { GoodTillCancel, ImmediateOrCancel };

enum class OrderStatus : std::uint8_t {
    Rested,                    // no fill, full quantity resting
    Filled,                    // fully filled on entry
    PartiallyFilledRested,     // some fills, remainder resting
    PartiallyFilledCancelled,  // some fills, remainder cancelled (IOC / market)
    Cancelled,                 // IOC / market with nothing to match
    RejectedInvalid,           // zero quantity
    RejectedDuplicateId,
    RejectedCapacity,          // pool or index exhausted
};

struct SubmitResult {
    OrderStatus status;
    Qty filled;
    Qty resting;
};

// Single-instrument price-time priority matching engine.
template <FillSink Sink>
class MatchingEngine {
public:
    struct Config {
        std::size_t max_orders = 1U << 20;
        std::size_t max_levels = 1U << 16;
        std::size_t level_reserve = 1024;
    };

    MatchingEngine(const Config& cfg, Sink& sink)
        : arena_(cfg.max_orders, cfg.max_levels),
          book_(arena_, cfg.level_reserve),
          index_(cfg.max_orders),
          sink_(&sink) {}

    [[nodiscard]] SubmitResult submit_limit(OrderId id, Side side, Price limit, Qty qty,
                                            TimeInForce tif = TimeInForce::GoodTillCancel) noexcept {
        if (qty == 0) [[unlikely]] {
            return {OrderStatus::RejectedInvalid, 0, 0};
        }
        if (index_.find(id) != nullptr) [[unlikely]] {
            return {OrderStatus::RejectedDuplicateId, 0, 0};
        }
        const Qty filled = match(id, side, qty, [side, limit](Price resting) noexcept {
            return side == Side::Buy ? resting <= limit : resting >= limit;
        });
        const Qty left = qty - filled;
        if (left == 0) {
            return {OrderStatus::Filled, filled, 0};
        }
        if (tif == TimeInForce::ImmediateOrCancel) {
            return {filled > 0 ? OrderStatus::PartiallyFilledCancelled : OrderStatus::Cancelled, filled, 0};
        }
        Order* order = book_.add(id, side, limit, left);
        if (order == nullptr) [[unlikely]] {
            return {OrderStatus::RejectedCapacity, filled, 0};
        }
        if (!index_.insert(id, order)) [[unlikely]] {
            book_.remove(order);
            return {OrderStatus::RejectedCapacity, filled, 0};
        }
        return {filled > 0 ? OrderStatus::PartiallyFilledRested : OrderStatus::Rested, filled, left};
    }

    [[nodiscard]] SubmitResult submit_market(OrderId id, Side side, Qty qty) noexcept {
        if (qty == 0) [[unlikely]] {
            return {OrderStatus::RejectedInvalid, 0, 0};
        }
        const Qty filled = match(id, side, qty, [](Price) noexcept { return true; });
        if (filled == qty) {
            return {OrderStatus::Filled, filled, 0};
        }
        return {filled > 0 ? OrderStatus::PartiallyFilledCancelled : OrderStatus::Cancelled, filled, 0};
    }

    bool cancel(OrderId id) noexcept {
        Order* order = index_.erase(id);
        if (order == nullptr) {
            return false;
        }
        book_.remove(order);
        return true;
    }

    // Quantity-down modify keeps time priority (exchange convention).
    // Increases must go through cancel + new order, which loses priority.
    bool reduce_to(OrderId id, Qty new_qty) noexcept {
        Order* order = index_.find(id);
        if (order == nullptr || new_qty >= order->qty) {
            return false;
        }
        if (new_qty == 0) {
            return cancel(id);
        }
        book_.reduce(order, order->qty - new_qty);
        return true;
    }

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] std::size_t resting_orders() const noexcept { return index_.size(); }

private:
    template <typename Crosses>
    Qty match(OrderId taker, Side side, Qty qty, Crosses crosses) noexcept {
        Qty filled = 0;
        const Side maker_side = opposite(side);
        while (filled < qty) {
            PriceLevel* lvl = book_.best_level(maker_side);
            if (lvl == nullptr || !crosses(lvl->price)) {
                break;
            }
            Order* maker = lvl->orders.front();
            const Qty trade = std::min<Qty>(qty - filled, maker->qty);
            sink_->on_fill(Fill{taker, maker->id, side, lvl->price, trade});
            filled += trade;
            if (trade == maker->qty) {
                index_.erase(maker->id);
                book_.remove(maker);  // may destroy lvl; re-read best next loop
            } else {
                book_.reduce(maker, trade);
            }
        }
        return filled;
    }

    BookArena arena_;
    OrderBook book_;
    OrderIndex<Order> index_;
    Sink* sink_;
};

}  // namespace limitbook
