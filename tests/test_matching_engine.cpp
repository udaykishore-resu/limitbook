#include <gtest/gtest.h>

#include "print_helpers.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "limitbook/matching_engine.hpp"
#include "limitbook/sim/rng.hpp"

using limitbook::Fill;
using limitbook::MatchingEngine;
using limitbook::OrderId;
using limitbook::OrderStatus;
using limitbook::Price;
using limitbook::Qty;
using limitbook::Quote;
using limitbook::Side;
using limitbook::TimeInForce;

namespace {

struct RecordingSink {
    std::vector<Fill> fills;
    void on_fill(const Fill& f) { fills.push_back(f); }
};
static_assert(limitbook::FillSink<RecordingSink>);

using Engine = MatchingEngine<RecordingSink>;

Engine::Config small_config() {
    Engine::Config cfg;
    cfg.max_orders = 1 << 12;
    cfg.max_levels = 1 << 12;
    cfg.level_reserve = 64;
    return cfg;
}

}  // namespace

TEST(MatchingEngine, RestsWhenNotCrossing) {
    RecordingSink sink;
    Engine eng{small_config(), sink};
    auto r = eng.submit_limit(1, Side::Buy, 100, 10);
    EXPECT_EQ(r.status, OrderStatus::Rested);
    EXPECT_EQ(r.resting, 10U);
    EXPECT_TRUE(sink.fills.empty());
    EXPECT_EQ(eng.book().best_bid(), (Quote{100, 10, 1}));
}

TEST(MatchingEngine, FillsAtMakerPriceAcrossLevelsInPriceTimeOrder) {
    RecordingSink sink;
    Engine eng{small_config(), sink};
    (void)eng.submit_limit(1, Side::Sell, 101, 5);
    (void)eng.submit_limit(2, Side::Sell, 100, 3);
    (void)eng.submit_limit(3, Side::Sell, 100, 4);  // same price, later time

    auto r = eng.submit_limit(10, Side::Buy, 101, 10);
    EXPECT_EQ(r.status, OrderStatus::Filled);
    EXPECT_EQ(r.filled, 10U);
    ASSERT_EQ(sink.fills.size(), 3U);
    EXPECT_EQ(sink.fills[0], (Fill{10, 2, Side::Buy, 100, 3}));
    EXPECT_EQ(sink.fills[1], (Fill{10, 3, Side::Buy, 100, 4}));
    EXPECT_EQ(sink.fills[2], (Fill{10, 1, Side::Buy, 101, 3}));
    EXPECT_EQ(eng.book().best_ask(), (Quote{101, 2, 1}));
    EXPECT_EQ(eng.resting_orders(), 1U);
}

TEST(MatchingEngine, PartialFillRestsRemainder) {
    RecordingSink sink;
    Engine eng{small_config(), sink};
    (void)eng.submit_limit(1, Side::Buy, 99, 4);
    auto r = eng.submit_limit(2, Side::Sell, 99, 10);
    EXPECT_EQ(r.status, OrderStatus::PartiallyFilledRested);
    EXPECT_EQ(r.filled, 4U);
    EXPECT_EQ(r.resting, 6U);
    EXPECT_EQ(eng.book().best_ask(), (Quote{99, 6, 1}));
    EXPECT_FALSE(eng.book().best_bid().has_value());
}

TEST(MatchingEngine, IocAndMarketNeverRest) {
    RecordingSink sink;
    Engine eng{small_config(), sink};
    (void)eng.submit_limit(1, Side::Sell, 100, 2);
    auto ioc = eng.submit_limit(2, Side::Buy, 100, 5, TimeInForce::ImmediateOrCancel);
    EXPECT_EQ(ioc.status, OrderStatus::PartiallyFilledCancelled);
    EXPECT_EQ(ioc.filled, 2U);
    auto mkt = eng.submit_market(3, Side::Buy, 5);
    EXPECT_EQ(mkt.status, OrderStatus::Cancelled);
    EXPECT_EQ(eng.resting_orders(), 0U);
}

TEST(MatchingEngine, RejectsDuplicatesAndZeroQuantity) {
    RecordingSink sink;
    Engine eng{small_config(), sink};
    EXPECT_EQ(eng.submit_limit(1, Side::Buy, 100, 0).status, OrderStatus::RejectedInvalid);
    EXPECT_EQ(eng.submit_limit(1, Side::Buy, 100, 1).status, OrderStatus::Rested);
    EXPECT_EQ(eng.submit_limit(1, Side::Buy, 100, 1).status, OrderStatus::RejectedDuplicateId);
}

TEST(MatchingEngine, ReduceKeepsPriorityCancelRemoves) {
    RecordingSink sink;
    Engine eng{small_config(), sink};
    (void)eng.submit_limit(1, Side::Sell, 100, 10);
    (void)eng.submit_limit(2, Side::Sell, 100, 10);
    EXPECT_TRUE(eng.reduce_to(1, 3));
    EXPECT_FALSE(eng.reduce_to(1, 5));  // increase not allowed
    (void)eng.submit_market(9, Side::Buy, 3);
    ASSERT_EQ(sink.fills.size(), 1U);
    EXPECT_EQ(sink.fills[0].maker, 1U);  // still first in queue after reduce
    EXPECT_TRUE(eng.cancel(2));
    EXPECT_FALSE(eng.cancel(2));
    EXPECT_EQ(eng.resting_orders(), 0U);
}

namespace {

// Naive oracle: std::map of std::deque, written to be obviously correct.
class NaiveMatcher {
public:
    std::vector<Fill> fills;

    void submit(OrderId id, Side side, Price limit, Qty qty, bool ioc) {
        auto& opp = side == Side::Buy ? asks_ : bids_;
        while (qty > 0 && !opp.empty()) {
            auto lvl = side == Side::Buy ? opp.begin() : std::prev(opp.end());
            const bool crosses = side == Side::Buy ? lvl->first <= limit : lvl->first >= limit;
            if (!crosses) {
                break;
            }
            auto& q = lvl->second;
            auto& maker = q.front();
            const Qty t = std::min(qty, maker.qty);
            fills.push_back(Fill{id, maker.id, side, lvl->first, t});
            qty -= t;
            maker.qty -= t;
            if (maker.qty == 0) {
                where_.erase(maker.id);
                q.pop_front();
                if (q.empty()) {
                    opp.erase(lvl);
                }
            }
        }
        if (qty > 0 && !ioc) {
            (side == Side::Buy ? bids_ : asks_)[limit].push_back({id, qty});
            where_[id] = {side, limit};
        }
    }

    bool cancel(OrderId id) {
        auto it = where_.find(id);
        if (it == where_.end()) {
            return false;
        }
        auto& book = it->second.first == Side::Buy ? bids_ : asks_;
        auto& q = book[it->second.second];
        q.erase(std::find_if(q.begin(), q.end(), [&](const Resting& r) { return r.id == id; }));
        if (q.empty()) {
            book.erase(it->second.second);
        }
        where_.erase(it);
        return true;
    }

    std::optional<Quote> best(Side side) const {
        const auto& book = side == Side::Buy ? bids_ : asks_;
        if (book.empty()) {
            return std::nullopt;
        }
        const auto& [price, q] = side == Side::Buy ? *book.rbegin() : *book.begin();
        std::uint64_t total = 0;
        for (const auto& r : q) {
            total += r.qty;
        }
        return Quote{price, total, static_cast<std::uint32_t>(q.size())};
    }

    std::vector<OrderId> live() const {
        std::vector<OrderId> ids;
        for (const auto& kv : where_) {
            ids.push_back(kv.first);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

private:
    struct Resting {
        OrderId id;
        Qty qty;
    };
    std::map<Price, std::deque<Resting>> bids_;
    std::map<Price, std::deque<Resting>> asks_;
    std::unordered_map<OrderId, std::pair<Side, Price>> where_;
};

}  // namespace

// Differential test: the fill stream and top of book must be identical to
// the naive matcher for 200k random limit / IOC / cancel operations.
TEST(MatchingEngine, MatchesNaiveOracleUnderRandomFlow) {
    RecordingSink sink;
    Engine eng{small_config(), sink};
    NaiveMatcher oracle;
    limitbook::sim::Rng rng{99};
    OrderId next = 1;

    for (int step = 0; step < 200'000; ++step) {
        const auto r = rng.below(100);
        if (r < 70) {
            const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
            const Price limit = 1000 + static_cast<Price>(rng.below(20));
            const Qty qty = static_cast<Qty>(1 + rng.below(50));
            const bool ioc = rng.chance(15);
            (void)eng.submit_limit(next, side, limit, qty,
                                   ioc ? TimeInForce::ImmediateOrCancel : TimeInForce::GoodTillCancel);
            oracle.submit(next, side, limit, qty, ioc);
            ++next;
        } else {
            const auto ids = oracle.live();
            if (!ids.empty()) {
                const OrderId id = ids[rng.below(ids.size())];
                ASSERT_EQ(eng.cancel(id), oracle.cancel(id));
            }
        }
        ASSERT_EQ(sink.fills.size(), oracle.fills.size()) << "step " << step;
        if (step % 50 == 0) {
            ASSERT_EQ(eng.book().best_bid(), oracle.best(Side::Buy)) << "step " << step;
            ASSERT_EQ(eng.book().best_ask(), oracle.best(Side::Sell)) << "step " << step;
            ASSERT_FALSE(eng.book().crossed());
        }
    }
    EXPECT_EQ(sink.fills, oracle.fills);
}
