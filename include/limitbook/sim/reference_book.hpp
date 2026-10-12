#pragma once

// Deliberately naive L3 book built from std::map / std::list /
// std::unordered_map. It is the *oracle*: obviously correct, slow, and
// allocation-heavy. Tests diff the fast book against it message by message,
// and benchmarks use it as the baseline the fast book must beat.

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "limitbook/order_book.hpp"
#include "limitbook/sim/rng.hpp"
#include "limitbook/types.hpp"

namespace limitbook::sim {

class ReferenceBook {
public:
    struct Rec {
        Side side;
        Price price;
        Qty qty;
    };

    ReferenceBook() = default;
    // Entries hold std::list iterators into this object; a copy would alias
    // the source's lists. Moves keep node addresses, so they are safe.
    ReferenceBook(const ReferenceBook&) = delete;
    ReferenceBook& operator=(const ReferenceBook&) = delete;
    ReferenceBook(ReferenceBook&&) = default;
    ReferenceBook& operator=(ReferenceBook&&) = default;
    ~ReferenceBook() = default;

    bool add(OrderId id, Side side, Price price, Qty qty) {
        if (orders_.contains(id)) {
            return false;
        }
        Level& lvl = levels(side)[price];
        lvl.fifo.push_back(id);
        lvl.total += qty;
        orders_.emplace(id, Entry{Rec{side, price, qty}, std::prev(lvl.fifo.end()), live_.size()});
        live_.push_back(id);
        return true;
    }

    // Returns true if the order was fully removed.
    bool reduce(OrderId id, Qty by) {
        auto it = orders_.find(id);
        if (it == orders_.end()) {
            return false;
        }
        if (by >= it->second.rec.qty) {
            remove(id);
            return true;
        }
        it->second.rec.qty -= by;
        levels(it->second.rec.side)[it->second.rec.price].total -= by;
        return false;
    }

    bool remove(OrderId id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) {
            return false;
        }
        const Entry& e = it->second;
        auto& side_levels = levels(e.rec.side);
        auto lvl_it = side_levels.find(e.rec.price);
        lvl_it->second.fifo.erase(e.fifo_pos);
        lvl_it->second.total -= e.rec.qty;
        if (lvl_it->second.fifo.empty()) {
            side_levels.erase(lvl_it);
        }
        // O(1) swap-remove from the live vector.
        const std::size_t pos = e.live_pos;
        const OrderId last = live_.back();
        live_[pos] = last;
        if (auto moved = orders_.find(last); moved != orders_.end()) {
            moved->second.live_pos = pos;
        }
        live_.pop_back();
        orders_.erase(id);
        return true;
    }

    [[nodiscard]] std::optional<Rec> get(OrderId id) const {
        auto it = orders_.find(id);
        if (it == orders_.end()) {
            return std::nullopt;
        }
        return it->second.rec;
    }

    [[nodiscard]] std::optional<Quote> best(Side side) const {
        const auto& m = side == Side::Buy ? bids_ : asks_;
        if (m.empty()) {
            return std::nullopt;
        }
        const auto& [price, lvl] = side == Side::Buy ? *m.rbegin() : *m.begin();
        return Quote{price, lvl.total, static_cast<std::uint32_t>(lvl.fifo.size())};
    }

    // Oldest order at the best price on `side`.
    [[nodiscard]] std::optional<OrderId> front_at_best(Side side) const {
        const auto& m = side == Side::Buy ? bids_ : asks_;
        if (m.empty()) {
            return std::nullopt;
        }
        const auto& lvl = side == Side::Buy ? m.rbegin()->second : m.begin()->second;
        return lvl.fifo.front();
    }

    // Levels from best to worst.
    [[nodiscard]] std::vector<Quote> depth(Side side, std::size_t max_levels) const {
        std::vector<Quote> out;
        auto emit = [&](const auto& kv) {
            out.push_back(Quote{kv.first, kv.second.total, static_cast<std::uint32_t>(kv.second.fifo.size())});
        };
        if (side == Side::Buy) {
            for (auto it = bids_.rbegin(); it != bids_.rend() && out.size() < max_levels; ++it) {
                emit(*it);
            }
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && out.size() < max_levels; ++it) {
                emit(*it);
            }
        }
        return out;
    }

    [[nodiscard]] std::optional<OrderId> random_live(Rng& rng) const {
        if (live_.empty()) {
            return std::nullopt;
        }
        return live_[rng.below(live_.size())];
    }

    [[nodiscard]] std::size_t size() const noexcept { return orders_.size(); }

private:
    struct Level {
        std::list<OrderId> fifo;
        std::uint64_t total{0};
    };
    struct Entry {
        Rec rec;
        std::list<OrderId>::iterator fifo_pos;
        std::size_t live_pos;
    };

    std::map<Price, Level>& levels(Side side) { return side == Side::Buy ? bids_ : asks_; }

    std::map<Price, Level> bids_;
    std::map<Price, Level> asks_;
    std::unordered_map<OrderId, Entry> orders_;
    std::vector<OrderId> live_;
};

}  // namespace limitbook::sim
