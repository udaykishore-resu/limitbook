#include <gtest/gtest.h>

#include "print_helpers.hpp"

#include <memory>
#include <span>
#include <vector>

#include "limitbook/itch/book_builder.hpp"
#include "limitbook/itch/decoder.hpp"
#include "limitbook/itch/encoder.hpp"
#include "limitbook/sim/synthetic_feed.hpp"

namespace itch = limitbook::itch;
using limitbook::PriceLevel;
using limitbook::Quote;
using limitbook::Side;

namespace {

std::vector<Quote> depth(const limitbook::OrderBook& book, Side side) {
    std::vector<Quote> out;
    book.for_each_level(side, 10'000,
                        [&](const PriceLevel& l) { out.push_back({l.price, l.total_qty, l.order_count()}); });
    return out;
}

itch::BookBuilder::Config test_config() {
    itch::BookBuilder::Config cfg;
    cfg.max_orders = 1 << 16;
    cfg.max_levels = 1 << 14;
    cfg.level_reserve = 32;
    cfg.validate = true;
    return cfg;
}

}  // namespace

TEST(BookBuilder, AppliesHandWrittenSequence) {
    std::vector<std::byte> buf;
    itch::Encoder enc{buf};
    enc.stock_directory(5, 0, "AAPL");
    enc.add_order(5, 1, 100, Side::Buy, 300, "AAPL", 1'500'000);
    enc.add_order(5, 2, 101, Side::Buy, 200, "AAPL", 1'500'000);
    enc.add_order(5, 3, 102, Side::Sell, 100, "AAPL", 1'500'100);
    enc.order_executed(5, 4, 100, 50, 1);       // 300 -> 250
    enc.order_cancel(5, 5, 101, 200);           // full cancel removes 101
    enc.order_replace(5, 6, 102, 103, 400, 1'500'200);
    enc.order_delete(5, 7, 999);                // unknown ref

    auto builder = std::make_unique<itch::BookBuilder>(test_config());
    const auto stats = itch::for_each_framed(std::span<const std::byte>{buf}, *builder);
    ASSERT_EQ(stats.errors, 0U);

    const auto* book = builder->book(5);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(builder->symbol(5), "AAPL");
    EXPECT_EQ(builder->locate_of("AAPL"), 5);
    EXPECT_EQ(book->best_bid(), (Quote{1'500'000, 250, 1}));
    EXPECT_EQ(book->best_ask(), (Quote{1'500'200, 400, 1}));
    EXPECT_EQ(builder->live_orders(), 2U);
    EXPECT_EQ(builder->stats().unknown_refs, 1U);
    EXPECT_EQ(builder->stats().executions_off_top, 0U);
}

// End-to-end differential test: generate 1M messages of synthetic order flow,
// decode them into the BookBuilder, and require every symbol's full depth to
// equal the generator's std::map reference book.
TEST(BookBuilder, MatchesSyntheticFeedReference) {
    limitbook::sim::SyntheticFeed::Config fcfg;
    fcfg.symbols = 6;
    fcfg.seed = 12345;
    limitbook::sim::SyntheticFeed feed{fcfg};

    std::vector<std::byte> buf;
    itch::Encoder enc{buf};
    feed.emit_directory(enc);

    auto builder = std::make_unique<itch::BookBuilder>(test_config());
    for (int chunk = 0; chunk < 10; ++chunk) {
        feed.emit(enc, 100'000);
        const auto stats = itch::for_each_framed(std::span<const std::byte>{buf}, *builder);
        ASSERT_EQ(stats.errors, 0U);
        buf.clear();

        for (const auto& sym : feed.symbols()) {
            const auto* book = builder->book(sym.locate);
            ASSERT_NE(book, nullptr);
            ASSERT_EQ(depth(*book, Side::Buy), sym.book.depth(Side::Buy, 10'000)) << sym.name << " chunk " << chunk;
            ASSERT_EQ(depth(*book, Side::Sell), sym.book.depth(Side::Sell, 10'000)) << sym.name << " chunk " << chunk;
            EXPECT_FALSE(book->crossed());
        }
    }
    const auto& st = builder->stats();
    EXPECT_EQ(st.unknown_refs, 0U);
    EXPECT_EQ(st.capacity_failures, 0U);
    EXPECT_EQ(st.executions_off_top, 0U);
    EXPECT_EQ(st.crossed_after_add, 0U);
    EXPECT_GT(st.replaces, 0U);
    EXPECT_GT(st.executions, 0U);
}

TEST(BookBuilder, ReportsCapacityFailuresInsteadOfAllocating) {
    itch::BookBuilder::Config cfg;
    cfg.max_orders = 2;
    cfg.max_levels = 8;
    auto builder = std::make_unique<itch::BookBuilder>(cfg);
    std::vector<std::byte> buf;
    itch::Encoder enc{buf};
    enc.stock_directory(1, 0, "X");
    for (limitbook::OrderId id = 1; id <= 5; ++id) {
        enc.add_order(1, id, id, Side::Buy, 1, "X", 100);
    }
    (void)itch::for_each_framed(std::span<const std::byte>{buf}, *builder);
    EXPECT_EQ(builder->live_orders(), 2U);
    EXPECT_EQ(builder->stats().capacity_failures, 3U);
}
