#include <gtest/gtest.h>

#include "print_helpers.hpp"

#include <vector>

#include "limitbook/order_book.hpp"
#include "limitbook/order_index.hpp"
#include "limitbook/sim/reference_book.hpp"
#include "limitbook/sim/rng.hpp"

using limitbook::BookArena;
using limitbook::Order;
using limitbook::OrderBook;
using limitbook::OrderId;
using limitbook::Price;
using limitbook::PriceLevel;
using limitbook::Qty;
using limitbook::Quote;
using limitbook::Side;

namespace {

std::vector<Quote> depth(const OrderBook& book, Side side, std::size_t n = 1000) {
    std::vector<Quote> out;
    book.for_each_level(side, n, [&](const PriceLevel& l) { out.push_back({l.price, l.total_qty, l.order_count()}); });
    return out;
}

}  // namespace

TEST(OrderBook, BestPricesAndAggregation) {
    BookArena arena{64, 64};
    OrderBook book{arena};
    ASSERT_NE(book.add(1, Side::Buy, 100, 10), nullptr);
    ASSERT_NE(book.add(2, Side::Buy, 101, 5), nullptr);
    ASSERT_NE(book.add(3, Side::Buy, 101, 7), nullptr);
    ASSERT_NE(book.add(4, Side::Sell, 103, 4), nullptr);
    ASSERT_NE(book.add(5, Side::Sell, 102, 9), nullptr);

    EXPECT_EQ(book.best_bid(), (Quote{101, 12, 2}));
    EXPECT_EQ(book.best_ask(), (Quote{102, 9, 1}));
    EXPECT_FALSE(book.crossed());
    EXPECT_EQ(depth(book, Side::Buy), (std::vector<Quote>{{101, 12, 2}, {100, 10, 1}}));
    EXPECT_EQ(depth(book, Side::Sell), (std::vector<Quote>{{102, 9, 1}, {103, 4, 1}}));
}

TEST(OrderBook, TimePriorityWithinLevel) {
    BookArena arena{8, 8};
    OrderBook book{arena};
    Order* first = book.add(1, Side::Sell, 50, 1);
    Order* second = book.add(2, Side::Sell, 50, 1);
    EXPECT_EQ(book.best_level(Side::Sell)->orders.front(), first);
    book.remove(first);
    EXPECT_EQ(book.best_level(Side::Sell)->orders.front(), second);
}

TEST(OrderBook, ReduceAndRemoveFreeLevelsAndOrders) {
    BookArena arena{8, 8};
    OrderBook book{arena};
    Order* o = book.add(1, Side::Buy, 10, 100);
    EXPECT_FALSE(book.reduce(o, 40));
    EXPECT_EQ(book.best_bid(), (Quote{10, 60, 1}));
    EXPECT_TRUE(book.reduce(o, 60));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(arena.orders.live(), 0U);
    EXPECT_EQ(arena.levels.live(), 0U);
}

TEST(OrderBook, ExhaustedArenaLeavesBookUnchanged) {
    BookArena arena{2, 1};
    OrderBook book{arena};
    ASSERT_NE(book.add(1, Side::Buy, 10, 1), nullptr);
    EXPECT_EQ(book.add(2, Side::Buy, 11, 1), nullptr);  // no level slot left
    EXPECT_EQ(arena.orders.live(), 1U);
    EXPECT_EQ(depth(book, Side::Buy), (std::vector<Quote>{{10, 1, 1}}));
}

TEST(OrderBook, DestructorReturnsEverythingToArena) {
    BookArena arena{128, 128};
    {
        OrderBook book{arena};
        for (OrderId i = 0; i < 100; ++i) {
            ASSERT_NE(book.add(i, i % 2 ? Side::Buy : Side::Sell, static_cast<Price>(i % 2 ? 100 - i % 10 : 200 + i % 10), 1),
                      nullptr);
        }
    }
    EXPECT_EQ(arena.orders.live(), 0U);
    EXPECT_EQ(arena.levels.live(), 0U);
}

// Differential property test: 300k random operations applied to the fast
// book and to the std::map oracle; full depth must agree throughout.
TEST(OrderBook, MatchesReferenceBookUnderRandomFlow) {
    constexpr std::size_t kMax = 1 << 14;
    BookArena arena{kMax, kMax};
    OrderBook book{arena, 16};  // small reserve: exercise vector growth too
    limitbook::OrderIndex<Order> index{kMax};
    limitbook::sim::ReferenceBook ref;
    limitbook::sim::Rng rng{2026};
    OrderId next_id = 1;

    for (int step = 0; step < 300'000; ++step) {
        const auto r = rng.below(100);
        // Grow toward ~2000 live orders, then churn around that depth.
        if (ref.size() < 50 || (ref.size() < 2000 ? r < 55 : r < 35)) {
            const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
            // Overlapping ranges: this book does not match, so crossing is fine.
            const Price price = 1000 + static_cast<Price>(rng.below(60));
            const Qty qty = static_cast<Qty>(1 + rng.below(500));
            Order* o = book.add(next_id, side, price, qty);
            ASSERT_NE(o, nullptr);
            ASSERT_TRUE(index.insert(next_id, o));
            ref.add(next_id, side, price, qty);
            ++next_id;
        } else if (r < 80) {
            const OrderId id = *ref.random_live(rng);
            book.remove(index.erase(id));
            ref.remove(id);
        } else {
            const OrderId id = *ref.random_live(rng);
            const Qty by = static_cast<Qty>(1 + rng.below(ref.get(id)->qty));
            if (book.reduce(index.find(id), by)) {
                index.erase(id);
            }
            ref.reduce(id, by);
        }
        if (step % 101 == 0) {
            ASSERT_EQ(depth(book, Side::Buy), ref.depth(Side::Buy, 1000)) << "step " << step;
            ASSERT_EQ(depth(book, Side::Sell), ref.depth(Side::Sell, 1000)) << "step " << step;
        }
    }
    EXPECT_EQ(index.size(), ref.size());
    EXPECT_EQ(arena.orders.live(), ref.size());
}
