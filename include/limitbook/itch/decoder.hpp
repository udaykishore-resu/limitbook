#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "limitbook/endian.hpp"
#include "limitbook/itch/messages.hpp"

namespace limitbook::itch {

enum class DecodeError : std::uint8_t { None, Empty, Truncated, UnknownType };

namespace detail {

template <typename M, typename Handler>
[[nodiscard]] inline DecodeError dispatch(std::span<const std::byte> msg, Handler& handler) noexcept {
    if (msg.size() < M::kSize) [[unlikely]] {
        return DecodeError::Truncated;
    }
    // Compile-time opt-in: a handler only pays for the messages it overloads.
    // Unhandled types compile down to the length check above.
    if constexpr (std::is_invocable_v<Handler&, const M&>) {
        handler(M{msg.data()});
    }
    return DecodeError::None;
}

}  // namespace detail

// Decode one unframed ITCH message and invoke `handler(const T&)` for the
// matching view type, if the handler accepts it.
template <typename Handler>
[[nodiscard]] inline DecodeError decode(std::span<const std::byte> msg, Handler& handler) noexcept {
    if (msg.empty()) [[unlikely]] {
        return DecodeError::Empty;
    }
    switch (static_cast<char>(msg[0])) {
        case AddOrder::kType: return detail::dispatch<AddOrder>(msg, handler);
        case OrderExecuted::kType: return detail::dispatch<OrderExecuted>(msg, handler);
        case OrderCancel::kType: return detail::dispatch<OrderCancel>(msg, handler);
        case OrderDelete::kType: return detail::dispatch<OrderDelete>(msg, handler);
        case OrderReplace::kType: return detail::dispatch<OrderReplace>(msg, handler);
        case AddOrderMpid::kType: return detail::dispatch<AddOrderMpid>(msg, handler);
        case OrderExecutedWithPrice::kType: return detail::dispatch<OrderExecutedWithPrice>(msg, handler);
        case Trade::kType: return detail::dispatch<Trade>(msg, handler);
        case StockDirectory::kType: return detail::dispatch<StockDirectory>(msg, handler);
        case SystemEvent::kType: return detail::dispatch<SystemEvent>(msg, handler);
        default: {
            const std::size_t expected = message_size(static_cast<char>(msg[0]));
            if (expected == 0) [[unlikely]] {
                return DecodeError::UnknownType;
            }
            return msg.size() < expected ? DecodeError::Truncated : DecodeError::None;
        }
    }
}

struct FrameStats {
    std::uint64_t messages{0};
    std::uint64_t bytes{0};
    std::uint64_t errors{0};
    bool truncated_tail{false};
};

// Walk a buffer of length-prefixed messages (u16 big-endian length, then the
// message). This is the framing of NASDAQ's binary ITCH files and of the
// message blocks inside a MoldUDP64 packet.
template <typename Handler>
inline FrameStats for_each_framed(std::span<const std::byte> buf, Handler&& handler) noexcept {
    FrameStats stats;
    std::size_t off = 0;
    while (off + 2 <= buf.size()) {
        const std::size_t len = load_be<std::uint16_t>(buf.data() + off);
        off += 2;
        if (off + len > buf.size()) [[unlikely]] {
            stats.truncated_tail = true;
            break;
        }
        if (decode(buf.subspan(off, len), handler) != DecodeError::None) [[unlikely]] {
            ++stats.errors;
        }
        off += len;
        ++stats.messages;
    }
    stats.bytes = off;
    return stats;
}

}  // namespace limitbook::itch
