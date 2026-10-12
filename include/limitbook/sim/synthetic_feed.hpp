#pragma once

// Deterministic synthetic ITCH 5.0 order flow.
//
// Not a market model: it exists so tests, benchmarks and CI have a realistic
// *shape* of traffic (adds clustered near the touch, cancels dominating,
// executions only at the top of book, replaces) without the 6+ GB NASDAQ
// sample file. Every symbol carries a ReferenceBook, so the generator also
// produces the expected book state for differential testing.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "limitbook/itch/encoder.hpp"
#include "limitbook/sim/reference_book.hpp"
#include "limitbook/sim/rng.hpp"
#include "limitbook/types.hpp"

namespace limitbook::sim {

class SyntheticFeed {
public:
    struct Config {
        std::uint16_t symbols = 4;
        std::uint64_t seed = 42;
        Price start_mid = 1'000'000;  // $100.0000 in ITCH 1e-4 units
        Price tick = 100;             // $0.01
        std::uint64_t max_ticks_from_touch = 40;
        std::uint64_t drift_every = 2'000;  // messages between mid moves
        std::size_t target_depth = 400;     // live orders per symbol to hover around
    };

    struct Symbol {
        std::string name;
        std::uint16_t locate;
        Price mid;
        ReferenceBook book;
    };

    explicit SyntheticFeed(const Config& cfg) : cfg_(cfg), rng_(cfg.seed) {
        static constexpr const char* kNames[] = {"AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA", "AMD"};
        constexpr std::size_t kNameCount = sizeof(kNames) / sizeof(kNames[0]);
        symbols_.reserve(cfg_.symbols);
        for (std::uint16_t i = 0; i < cfg_.symbols; ++i) {
            std::string name = i < kNameCount ? std::string{kNames[i]} : "SYM" + std::to_string(i);
            symbols_.push_back(Symbol{std::move(name), static_cast<std::uint16_t>(i + 1),
                                      cfg_.start_mid + static_cast<Price>(i) * 50 * cfg_.tick, ReferenceBook{}});
        }
    }

    void emit_directory(itch::Encoder& enc) {
        enc.system_event(0, ts_, 'O');
        for (const auto& s : symbols_) {
            enc.stock_directory(s.locate, ts_, s.name);
        }
        enc.system_event(0, ts_, 'Q');  // start of market hours
    }

    void emit(itch::Encoder& enc, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            ts_ += 1 + rng_.below(2'000);
            Symbol& s = symbols_[rng_.below(symbols_.size())];
            if (cfg_.drift_every > 0 && (++emitted_ % cfg_.drift_every) == 0) {
                s.mid += rng_.chance(50) ? cfg_.tick : -cfg_.tick;
            }
            step(enc, s);
        }
    }

    [[nodiscard]] const std::vector<Symbol>& symbols() const noexcept { return symbols_; }
    [[nodiscard]] std::uint64_t timestamp() const noexcept { return ts_; }

private:
    void step(itch::Encoder& enc, Symbol& s) {
        const std::size_t live = s.book.size();
        // Below target depth adds outpace removals (50% vs ~37%); above it
        // they lag (30% vs ~37%), so each book hovers around target_depth.
        const std::uint64_t add_pct = live < cfg_.target_depth ? 50 : 30;
        const std::uint64_t r = rng_.below(100);
        if (live < cfg_.target_depth / 4 || r < add_pct) {
            add(enc, s);
        } else if (r < add_pct + 25) {
            const OrderId id = *s.book.random_live(rng_);
            enc.order_delete(s.locate, ts_, id);
            s.book.remove(id);
        } else if (r < add_pct + 37) {
            const OrderId id = *s.book.random_live(rng_);
            const Qty qty = s.book.get(id)->qty;
            const Qty by = qty > 1 ? static_cast<Qty>(1 + rng_.below(qty - 1)) : 1;
            enc.order_cancel(s.locate, ts_, id, by);
            s.book.reduce(id, by);
        } else if (r < add_pct + 55) {
            execute_at_top(enc, s);
        } else {
            replace(enc, s);
        }
    }

    Qty shares() {
        return rng_.chance(85) ? static_cast<Qty>(100 * (1 + rng_.below(10))) : static_cast<Qty>(1 + rng_.below(99));
    }

    // Distance from the touch skewed toward zero: min of two uniforms.
    Price price_for(const Symbol& s, Side side) {
        const auto d = static_cast<Price>(
            std::min(rng_.below(cfg_.max_ticks_from_touch), rng_.below(cfg_.max_ticks_from_touch)));
        if (side == Side::Buy) {
            Price p = s.mid - (1 + d) * cfg_.tick;
            if (auto ask = s.book.best(Side::Sell); ask && p >= ask->price) {
                p = ask->price - cfg_.tick;  // never cross the book
            }
            return p;
        }
        Price p = s.mid + (1 + d) * cfg_.tick;
        if (auto bid = s.book.best(Side::Buy); bid && p <= bid->price) {
            p = bid->price + cfg_.tick;
        }
        return p;
    }

    void add(itch::Encoder& enc, Symbol& s) {
        const Side side = rng_.chance(50) ? Side::Buy : Side::Sell;
        const Price price = price_for(s, side);
        const Qty qty = shares();
        const OrderId id = next_id_++;
        if (rng_.chance(10)) {
            enc.add_order_mpid(s.locate, ts_, id, side, qty, s.name, static_cast<std::uint32_t>(price), "MMID");
        } else {
            enc.add_order(s.locate, ts_, id, side, qty, s.name, static_cast<std::uint32_t>(price));
        }
        s.book.add(id, side, price, qty);
    }

    void execute_at_top(itch::Encoder& enc, Symbol& s) {
        Side side = rng_.chance(50) ? Side::Buy : Side::Sell;
        auto id = s.book.front_at_best(side);
        if (!id) {
            side = opposite(side);
            id = s.book.front_at_best(side);
        }
        if (!id) {
            add(enc, s);
            return;
        }
        const Qty qty = s.book.get(*id)->qty;
        const Qty exec = rng_.chance(60) ? qty : static_cast<Qty>(1 + rng_.below(qty));
        enc.order_executed(s.locate, ts_, *id, exec, ++match_);
        s.book.reduce(*id, exec);
    }

    void replace(itch::Encoder& enc, Symbol& s) {
        const OrderId old_id = *s.book.random_live(rng_);
        const Side side = s.book.get(old_id)->side;
        s.book.remove(old_id);  // remove first so price_for sees the post-replace book
        const Price price = price_for(s, side);
        const Qty qty = shares();
        const OrderId new_id = next_id_++;
        enc.order_replace(s.locate, ts_, old_id, new_id, qty, static_cast<std::uint32_t>(price));
        s.book.add(new_id, side, price, qty);
    }

    Config cfg_;
    Rng rng_;
    std::vector<Symbol> symbols_;
    std::uint64_t ts_{34'200'000'000'000ULL};  // 09:30:00.000 in ns since midnight
    std::uint64_t emitted_{0};
    std::uint64_t match_{0};
    OrderId next_id_{1};
};

}  // namespace limitbook::sim
