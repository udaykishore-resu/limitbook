#pragma once

#include <cstdint>

namespace limitbook::sim {

// __extension__ silences -Wpedantic for the GCC/Clang 128-bit integer.
__extension__ using uint128 = unsigned __int128;

// xoshiro256** seeded by splitmix64.
//
// Why not std::mt19937 + std::uniform_int_distribution? The engine is
// portable but the *distributions are implementation-defined*: libstdc++ and
// libc++ produce different streams from the same seed. A synthetic feed that
// changes between your Mac and Linux CI is useless as a regression fixture.
class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed) noexcept {
        for (auto& word : s_) {
            word = splitmix64(seed);
        }
    }

    constexpr std::uint64_t next() noexcept {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform integer in [0, bound) via Lemire's multiply-shift (bias is
    // negligible for the small bounds a simulator uses).
    constexpr std::uint64_t below(std::uint64_t bound) noexcept {
        return static_cast<std::uint64_t>((static_cast<uint128>(next()) * bound) >> 64);
    }

    constexpr bool chance(std::uint64_t percent) noexcept { return below(100) < percent; }

private:
    static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    static constexpr std::uint64_t splitmix64(std::uint64_t& x) noexcept {
        std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    std::uint64_t s_[4]{};
};

}  // namespace limitbook::sim
