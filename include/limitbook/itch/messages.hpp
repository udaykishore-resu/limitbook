#pragma once

// NASDAQ TotalView-ITCH 5.0 message views.
//
// Each type is a zero-copy view over the wire bytes: no parsing step, no copy,
// no allocation. Field offsets are compile-time constants, so an accessor
// compiles to one unaligned load plus a byte swap. All integers are
// big-endian; prices are u32 with 4 implied decimal places.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "limitbook/endian.hpp"
#include "limitbook/types.hpp"

namespace limitbook::itch {

// Every ITCH 5.0 message type and its fixed length (0 = unknown type).
[[nodiscard]] constexpr std::size_t message_size(char type) noexcept {
    switch (type) {
        case 'S': return 12;  // System Event
        case 'R': return 39;  // Stock Directory
        case 'H': return 25;  // Stock Trading Action
        case 'Y': return 20;  // Reg SHO Restriction
        case 'L': return 26;  // Market Participant Position
        case 'V': return 35;  // MWCB Decline Level
        case 'W': return 12;  // MWCB Status
        case 'K': return 28;  // IPO Quoting Period Update
        case 'J': return 35;  // LULD Auction Collar
        case 'h': return 21;  // Operational Halt
        case 'A': return 36;  // Add Order (no MPID)
        case 'F': return 40;  // Add Order with MPID
        case 'E': return 31;  // Order Executed
        case 'C': return 36;  // Order Executed With Price
        case 'X': return 23;  // Order Cancel
        case 'D': return 19;  // Order Delete
        case 'U': return 35;  // Order Replace
        case 'P': return 44;  // Trade (non-cross)
        case 'Q': return 40;  // Cross Trade
        case 'B': return 19;  // Broken Trade
        case 'I': return 50;  // NOII
        case 'N': return 20;  // Retail Price Improvement Indicator
        case 'O': return 48;  // Direct Listing with Capital Raise
        default: return 0;
    }
}

// Common 11-byte header: type(1) locate(2) tracking(2) timestamp(6).
class MessageView {
public:
    explicit constexpr MessageView(const std::byte* data) noexcept : p_(data) {}

    [[nodiscard]] char type() const noexcept { return static_cast<char>(p_[0]); }
    [[nodiscard]] std::uint16_t stock_locate() const noexcept { return load_be<std::uint16_t>(p_ + 1); }
    [[nodiscard]] std::uint16_t tracking_number() const noexcept { return load_be<std::uint16_t>(p_ + 3); }
    [[nodiscard]] std::uint64_t timestamp() const noexcept { return load_be48(p_ + 5); }
    [[nodiscard]] const std::byte* data() const noexcept { return p_; }

protected:
    [[nodiscard]] std::uint16_t u16(std::size_t off) const noexcept { return load_be<std::uint16_t>(p_ + off); }
    [[nodiscard]] std::uint32_t u32(std::size_t off) const noexcept { return load_be<std::uint32_t>(p_ + off); }
    [[nodiscard]] std::uint64_t u64(std::size_t off) const noexcept { return load_be<std::uint64_t>(p_ + off); }
    [[nodiscard]] char ch(std::size_t off) const noexcept { return static_cast<char>(p_[off]); }
    [[nodiscard]] std::string_view alpha(std::size_t off, std::size_t len) const noexcept {
        // Alpha fields are left-justified and space-padded; trim the padding.
        const auto* s = reinterpret_cast<const char*>(p_ + off);
        while (len > 0 && s[len - 1] == ' ') {
            --len;
        }
        return {s, len};
    }

private:
    const std::byte* p_;
};

struct SystemEvent : MessageView {
    static constexpr char kType = 'S';
    static constexpr std::size_t kSize = 12;
    using MessageView::MessageView;
    [[nodiscard]] char event_code() const noexcept { return ch(11); }
};

struct StockDirectory : MessageView {
    static constexpr char kType = 'R';
    static constexpr std::size_t kSize = 39;
    using MessageView::MessageView;
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(11, 8); }
    [[nodiscard]] char market_category() const noexcept { return ch(19); }
    [[nodiscard]] std::uint32_t round_lot_size() const noexcept { return u32(21); }
};

struct AddOrder : MessageView {
    static constexpr char kType = 'A';
    static constexpr std::size_t kSize = 36;
    using MessageView::MessageView;
    [[nodiscard]] OrderId order_ref() const noexcept { return u64(11); }
    [[nodiscard]] Side side() const noexcept { return ch(19) == 'B' ? Side::Buy : Side::Sell; }
    [[nodiscard]] Qty shares() const noexcept { return u32(20); }
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(24, 8); }
    [[nodiscard]] std::uint32_t price() const noexcept { return u32(32); }
};

struct AddOrderMpid : MessageView {
    static constexpr char kType = 'F';
    static constexpr std::size_t kSize = 40;
    using MessageView::MessageView;
    [[nodiscard]] OrderId order_ref() const noexcept { return u64(11); }
    [[nodiscard]] Side side() const noexcept { return ch(19) == 'B' ? Side::Buy : Side::Sell; }
    [[nodiscard]] Qty shares() const noexcept { return u32(20); }
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(24, 8); }
    [[nodiscard]] std::uint32_t price() const noexcept { return u32(32); }
    [[nodiscard]] std::string_view attribution() const noexcept { return alpha(36, 4); }
};

struct OrderExecuted : MessageView {
    static constexpr char kType = 'E';
    static constexpr std::size_t kSize = 31;
    using MessageView::MessageView;
    [[nodiscard]] OrderId order_ref() const noexcept { return u64(11); }
    [[nodiscard]] Qty executed_shares() const noexcept { return u32(19); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return u64(23); }
};

struct OrderExecutedWithPrice : MessageView {
    static constexpr char kType = 'C';
    static constexpr std::size_t kSize = 36;
    using MessageView::MessageView;
    [[nodiscard]] OrderId order_ref() const noexcept { return u64(11); }
    [[nodiscard]] Qty executed_shares() const noexcept { return u32(19); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return u64(23); }
    [[nodiscard]] bool printable() const noexcept { return ch(31) == 'Y'; }
    [[nodiscard]] std::uint32_t execution_price() const noexcept { return u32(32); }
};

struct OrderCancel : MessageView {
    static constexpr char kType = 'X';
    static constexpr std::size_t kSize = 23;
    using MessageView::MessageView;
    [[nodiscard]] OrderId order_ref() const noexcept { return u64(11); }
    [[nodiscard]] Qty cancelled_shares() const noexcept { return u32(19); }
};

struct OrderDelete : MessageView {
    static constexpr char kType = 'D';
    static constexpr std::size_t kSize = 19;
    using MessageView::MessageView;
    [[nodiscard]] OrderId order_ref() const noexcept { return u64(11); }
};

struct OrderReplace : MessageView {
    static constexpr char kType = 'U';
    static constexpr std::size_t kSize = 35;
    using MessageView::MessageView;
    [[nodiscard]] OrderId original_order_ref() const noexcept { return u64(11); }
    [[nodiscard]] OrderId new_order_ref() const noexcept { return u64(19); }
    [[nodiscard]] Qty shares() const noexcept { return u32(27); }
    [[nodiscard]] std::uint32_t price() const noexcept { return u32(31); }
};

struct Trade : MessageView {
    static constexpr char kType = 'P';
    static constexpr std::size_t kSize = 44;
    using MessageView::MessageView;
    [[nodiscard]] OrderId order_ref() const noexcept { return u64(11); }
    [[nodiscard]] Side side() const noexcept { return ch(19) == 'B' ? Side::Buy : Side::Sell; }
    [[nodiscard]] Qty shares() const noexcept { return u32(20); }
    [[nodiscard]] std::string_view stock() const noexcept { return alpha(24, 8); }
    [[nodiscard]] std::uint32_t price() const noexcept { return u32(32); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return u64(36); }
};

// Compile-time proof that every view agrees with the spec table.
static_assert(SystemEvent::kSize == message_size(SystemEvent::kType));
static_assert(StockDirectory::kSize == message_size(StockDirectory::kType));
static_assert(AddOrder::kSize == message_size(AddOrder::kType));
static_assert(AddOrderMpid::kSize == message_size(AddOrderMpid::kType));
static_assert(OrderExecuted::kSize == message_size(OrderExecuted::kType));
static_assert(OrderExecutedWithPrice::kSize == message_size(OrderExecutedWithPrice::kType));
static_assert(OrderCancel::kSize == message_size(OrderCancel::kType));
static_assert(OrderDelete::kSize == message_size(OrderDelete::kType));
static_assert(OrderReplace::kSize == message_size(OrderReplace::kType));
static_assert(Trade::kSize == message_size(Trade::kType));

}  // namespace limitbook::itch
