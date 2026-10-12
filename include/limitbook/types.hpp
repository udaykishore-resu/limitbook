#pragma once

#include <cstddef>
#include <cstdint>

namespace limitbook {

// Prices are integer ticks. ITCH 5.0 prices are u32 with 4 implied decimals,
// so $187.2300 arrives as 1'872'300. Never use floating point for prices on
// the hot path: equality and ordering must be exact.
using Price = std::int64_t;
using Qty = std::uint32_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// std::hardware_destructive_interference_size is not portable in headers
// (GCC warns with -Winterference-size, and its value is an ABI hazard).
// Apple Silicon uses 128-byte cache lines; x86-64 uses 64.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

}  // namespace limitbook
