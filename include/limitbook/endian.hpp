#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace limitbook {

// std::byteswap is C++23. Feature-test so the same code uses the standard
// facility where available and the compiler builtin everywhere else.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T byteswap(T v) noexcept {
#if defined(__cpp_lib_byteswap)
    return std::byteswap(v);
#else
    if constexpr (sizeof(T) == 1) {
        return v;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(static_cast<std::uint16_t>(v)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(static_cast<std::uint32_t>(v)));
    } else {
        static_assert(sizeof(T) == 8, "unsupported width");
        return static_cast<T>(__builtin_bswap64(static_cast<std::uint64_t>(v)));
    }
#endif
}

// memcpy is the only portable way to read an unaligned field without UB.
// At -O2 every mainstream compiler lowers it to a single load (+ bswap/rev).
template <std::unsigned_integral T>
[[nodiscard]] inline T load_be(const std::byte* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        return byteswap(v);
    } else {
        return v;
    }
}

// ITCH timestamps are 6-byte big-endian nanoseconds since midnight.
[[nodiscard]] inline std::uint64_t load_be48(const std::byte* p) noexcept {
    return (static_cast<std::uint64_t>(load_be<std::uint16_t>(p)) << 32) |
           static_cast<std::uint64_t>(load_be<std::uint32_t>(p + 2));
}

template <std::unsigned_integral T>
inline void store_be(std::byte* p, T v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        v = byteswap(v);
    }
    std::memcpy(p, &v, sizeof(T));
}

inline void store_be48(std::byte* p, std::uint64_t v) noexcept {
    store_be<std::uint16_t>(p, static_cast<std::uint16_t>(v >> 32));
    store_be<std::uint32_t>(p + 2, static_cast<std::uint32_t>(v & 0xFFFF'FFFFULL));
}

}  // namespace limitbook
