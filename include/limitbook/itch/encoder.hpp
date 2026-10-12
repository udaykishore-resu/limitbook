#pragma once

// ITCH 5.0 encoder for tests, benchmarks and the synthetic feed generator.
// Off the hot path: it appends to a std::vector and may allocate.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "limitbook/endian.hpp"
#include "limitbook/itch/messages.hpp"
#include "limitbook/types.hpp"

namespace limitbook::itch {

class Encoder {
public:
    // framed=true prefixes each message with its u16 length (file / MoldUDP64
    // framing); framed=false writes bare messages.
    explicit Encoder(std::vector<std::byte>& out, bool framed = true) : out_(&out), framed_(framed) {}

    void system_event(std::uint16_t locate, std::uint64_t ts, char code) {
        std::byte* p = begin(SystemEvent::kSize, SystemEvent::kType, locate, ts);
        p[11] = static_cast<std::byte>(code);
    }

    void stock_directory(std::uint16_t locate, std::uint64_t ts, std::string_view stock,
                         std::uint32_t round_lot = 100) {
        std::byte* p = begin(StockDirectory::kSize, StockDirectory::kType, locate, ts);
        put_alpha(p + 11, stock, 8);
        p[19] = static_cast<std::byte>('Q');  // market category
        p[20] = static_cast<std::byte>('N');  // financial status
        store_be<std::uint32_t>(p + 21, round_lot);
        for (std::size_t i = 25; i < StockDirectory::kSize; ++i) {
            p[i] = static_cast<std::byte>(' ');
        }
        store_be<std::uint32_t>(p + 34, 0);  // ETP leverage factor
    }

    void add_order(std::uint16_t locate, std::uint64_t ts, OrderId ref, Side side, Qty shares,
                   std::string_view stock, std::uint32_t price) {
        std::byte* p = begin(AddOrder::kSize, AddOrder::kType, locate, ts);
        write_add_body(p, ref, side, shares, stock, price);
    }

    void add_order_mpid(std::uint16_t locate, std::uint64_t ts, OrderId ref, Side side, Qty shares,
                        std::string_view stock, std::uint32_t price, std::string_view mpid) {
        std::byte* p = begin(AddOrderMpid::kSize, AddOrderMpid::kType, locate, ts);
        write_add_body(p, ref, side, shares, stock, price);
        put_alpha(p + 36, mpid, 4);
    }

    void order_executed(std::uint16_t locate, std::uint64_t ts, OrderId ref, Qty shares, std::uint64_t match) {
        std::byte* p = begin(OrderExecuted::kSize, OrderExecuted::kType, locate, ts);
        store_be<std::uint64_t>(p + 11, ref);
        store_be<std::uint32_t>(p + 19, shares);
        store_be<std::uint64_t>(p + 23, match);
    }

    void order_executed_with_price(std::uint16_t locate, std::uint64_t ts, OrderId ref, Qty shares,
                                   std::uint64_t match, bool printable, std::uint32_t price) {
        std::byte* p = begin(OrderExecutedWithPrice::kSize, OrderExecutedWithPrice::kType, locate, ts);
        store_be<std::uint64_t>(p + 11, ref);
        store_be<std::uint32_t>(p + 19, shares);
        store_be<std::uint64_t>(p + 23, match);
        p[31] = static_cast<std::byte>(printable ? 'Y' : 'N');
        store_be<std::uint32_t>(p + 32, price);
    }

    void order_cancel(std::uint16_t locate, std::uint64_t ts, OrderId ref, Qty shares) {
        std::byte* p = begin(OrderCancel::kSize, OrderCancel::kType, locate, ts);
        store_be<std::uint64_t>(p + 11, ref);
        store_be<std::uint32_t>(p + 19, shares);
    }

    void order_delete(std::uint16_t locate, std::uint64_t ts, OrderId ref) {
        std::byte* p = begin(OrderDelete::kSize, OrderDelete::kType, locate, ts);
        store_be<std::uint64_t>(p + 11, ref);
    }

    void order_replace(std::uint16_t locate, std::uint64_t ts, OrderId original, OrderId replacement, Qty shares,
                       std::uint32_t price) {
        std::byte* p = begin(OrderReplace::kSize, OrderReplace::kType, locate, ts);
        store_be<std::uint64_t>(p + 11, original);
        store_be<std::uint64_t>(p + 19, replacement);
        store_be<std::uint32_t>(p + 27, shares);
        store_be<std::uint32_t>(p + 31, price);
    }

    void trade(std::uint16_t locate, std::uint64_t ts, OrderId ref, Side side, Qty shares, std::string_view stock,
               std::uint32_t price, std::uint64_t match) {
        std::byte* p = begin(Trade::kSize, Trade::kType, locate, ts);
        write_add_body(p, ref, side, shares, stock, price);
        store_be<std::uint64_t>(p + 36, match);
    }

private:
    std::byte* begin(std::size_t size, char type, std::uint16_t locate, std::uint64_t ts) {
        const std::size_t start = out_->size();
        const std::size_t prefix = framed_ ? 2 : 0;
        out_->resize(start + prefix + size, std::byte{0});
        std::byte* frame = out_->data() + start;
        if (framed_) {
            store_be<std::uint16_t>(frame, static_cast<std::uint16_t>(size));
        }
        std::byte* p = frame + prefix;
        p[0] = static_cast<std::byte>(type);
        store_be<std::uint16_t>(p + 1, locate);
        store_be<std::uint16_t>(p + 3, 0);  // tracking number
        store_be48(p + 5, ts);
        return p;
    }

    static void write_add_body(std::byte* p, OrderId ref, Side side, Qty shares, std::string_view stock,
                               std::uint32_t price) {
        store_be<std::uint64_t>(p + 11, ref);
        p[19] = static_cast<std::byte>(side == Side::Buy ? 'B' : 'S');
        store_be<std::uint32_t>(p + 20, shares);
        put_alpha(p + 24, stock, 8);
        store_be<std::uint32_t>(p + 32, price);
    }

    static void put_alpha(std::byte* p, std::string_view s, std::size_t width) {
        const std::size_t n = std::min(s.size(), width);
        for (std::size_t i = 0; i < width; ++i) {
            p[i] = static_cast<std::byte>(i < n ? s[i] : ' ');
        }
    }

    std::vector<std::byte>* out_;
    bool framed_;
};

}  // namespace limitbook::itch
