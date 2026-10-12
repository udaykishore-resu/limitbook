#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "limitbook/endian.hpp"
#include "limitbook/itch/decoder.hpp"
#include "limitbook/itch/encoder.hpp"

namespace itch = limitbook::itch;
using limitbook::Side;

TEST(Endian, RoundTripsAllWidths) {
    std::byte buf[8]{};
    limitbook::store_be<std::uint16_t>(buf, 0x1234);
    EXPECT_EQ(buf[0], std::byte{0x12});
    EXPECT_EQ(limitbook::load_be<std::uint16_t>(buf), 0x1234);
    limitbook::store_be<std::uint32_t>(buf, 0xDEADBEEF);
    EXPECT_EQ(limitbook::load_be<std::uint32_t>(buf), 0xDEADBEEFU);
    limitbook::store_be<std::uint64_t>(buf, 0x0102030405060708ULL);
    EXPECT_EQ(buf[7], std::byte{0x08});
    EXPECT_EQ(limitbook::load_be<std::uint64_t>(buf), 0x0102030405060708ULL);
    limitbook::store_be48(buf, 0x0000A1B2C3D4E5F6ULL);
    EXPECT_EQ(limitbook::load_be48(buf), 0x0000A1B2C3D4E5F6ULL);
    static_assert(limitbook::byteswap<std::uint32_t>(0x11223344U) == 0x44332211U);
}

TEST(ItchCodec, AddOrderRoundTrip) {
    std::vector<std::byte> buf;
    itch::Encoder enc{buf, /*framed=*/false};
    enc.add_order(42, 34'200'000'000'123ULL, 987654321, Side::Sell, 300, "MSFT", 4'235'500);
    ASSERT_EQ(buf.size(), itch::AddOrder::kSize);

    bool seen = false;
    auto handler = [&](const itch::AddOrder& m) {
        seen = true;
        EXPECT_EQ(m.stock_locate(), 42);
        EXPECT_EQ(m.timestamp(), 34'200'000'000'123ULL);
        EXPECT_EQ(m.order_ref(), 987654321U);
        EXPECT_EQ(m.side(), Side::Sell);
        EXPECT_EQ(m.shares(), 300U);
        EXPECT_EQ(m.stock(), "MSFT");  // space padding trimmed
        EXPECT_EQ(m.price(), 4'235'500U);
    };
    EXPECT_EQ(itch::decode(std::span<const std::byte>{buf}, handler), itch::DecodeError::None);
    EXPECT_TRUE(seen);
}

TEST(ItchCodec, EveryOrderMessageRoundTrips) {
    std::vector<std::byte> buf;
    itch::Encoder enc{buf};
    enc.stock_directory(7, 1, "NVDA");
    enc.add_order_mpid(7, 2, 11, Side::Buy, 100, "NVDA", 1'000'000, "GSCO");
    enc.order_executed(7, 3, 11, 40, 555);
    enc.order_executed_with_price(7, 4, 11, 10, 556, true, 999'900);
    enc.order_cancel(7, 5, 11, 20);
    enc.order_replace(7, 6, 11, 12, 50, 1'000'100);
    enc.order_delete(7, 7, 12);
    enc.trade(7, 8, 0, Side::Buy, 25, "NVDA", 1'000'000, 557);

    std::string order;
    struct H {
        std::string* order;
        void operator()(const itch::StockDirectory& m) const {
            *order += 'R';
            EXPECT_EQ(m.stock(), "NVDA");
            EXPECT_EQ(m.round_lot_size(), 100U);
        }
        void operator()(const itch::AddOrderMpid& m) const {
            *order += 'F';
            EXPECT_EQ(m.attribution(), "GSCO");
            EXPECT_EQ(m.side(), Side::Buy);
        }
        void operator()(const itch::OrderExecuted& m) const {
            *order += 'E';
            EXPECT_EQ(m.executed_shares(), 40U);
            EXPECT_EQ(m.match_number(), 555U);
        }
        void operator()(const itch::OrderExecutedWithPrice& m) const {
            *order += 'C';
            EXPECT_TRUE(m.printable());
            EXPECT_EQ(m.execution_price(), 999'900U);
        }
        void operator()(const itch::OrderCancel& m) const {
            *order += 'X';
            EXPECT_EQ(m.cancelled_shares(), 20U);
        }
        void operator()(const itch::OrderReplace& m) const {
            *order += 'U';
            EXPECT_EQ(m.original_order_ref(), 11U);
            EXPECT_EQ(m.new_order_ref(), 12U);
            EXPECT_EQ(m.shares(), 50U);
            EXPECT_EQ(m.price(), 1'000'100U);
        }
        void operator()(const itch::OrderDelete& m) const {
            *order += 'D';
            EXPECT_EQ(m.order_ref(), 12U);
        }
        void operator()(const itch::Trade& m) const {
            *order += 'P';
            EXPECT_EQ(m.match_number(), 557U);
        }
    } handler{&order};

    const auto stats = itch::for_each_framed(std::span<const std::byte>{buf}, handler);
    EXPECT_EQ(order, "RFECXUDP");
    EXPECT_EQ(stats.messages, 8U);
    EXPECT_EQ(stats.errors, 0U);
    EXPECT_FALSE(stats.truncated_tail);
    EXPECT_EQ(stats.bytes, buf.size());
}

TEST(ItchCodec, HandlerOnlyPaysForTypesItAccepts) {
    std::vector<std::byte> buf;
    itch::Encoder enc{buf};
    enc.add_order(1, 1, 1, Side::Buy, 1, "A", 1);
    enc.order_delete(1, 2, 1);
    int deletes = 0;
    auto only_deletes = [&](const itch::OrderDelete&) { ++deletes; };
    const auto stats = itch::for_each_framed(std::span<const std::byte>{buf}, only_deletes);
    EXPECT_EQ(stats.messages, 2U);
    EXPECT_EQ(deletes, 1);
}

TEST(ItchCodec, DetectsTruncationAndUnknownTypes) {
    std::vector<std::byte> buf;
    itch::Encoder enc{buf, false};
    enc.add_order(1, 1, 1, Side::Buy, 1, "A", 1);
    auto noop = [](const itch::AddOrder&) {};
    const std::span<const std::byte> all{buf};
    EXPECT_EQ(itch::decode(all.first(20), noop), itch::DecodeError::Truncated);
    EXPECT_EQ(itch::decode(all.first(0), noop), itch::DecodeError::Empty);

    std::byte unknown[4]{std::byte{'Z'}};
    EXPECT_EQ(itch::decode(std::span<const std::byte>{unknown}, noop), itch::DecodeError::UnknownType);

    // Known-but-unhandled type of the right size decodes cleanly (skipped).
    std::vector<std::byte> halt(itch::message_size('H'), std::byte{0});
    halt[0] = std::byte{'H'};
    EXPECT_EQ(itch::decode(std::span<const std::byte>{halt}, noop), itch::DecodeError::None);

    // A frame whose length runs past the end of the buffer.
    std::vector<std::byte> framed;
    itch::Encoder fenc{framed};
    fenc.order_delete(1, 1, 1);
    framed.pop_back();
    const auto stats = itch::for_each_framed(std::span<const std::byte>{framed}, noop);
    EXPECT_TRUE(stats.truncated_tail);
    EXPECT_EQ(stats.messages, 0U);
}
