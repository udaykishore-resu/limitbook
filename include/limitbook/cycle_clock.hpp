#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace limitbook {

// Cheap timestamp counter for per-message latency measurement.
//
//  x86-64  : lfence + rdtsc. Invariant TSC on any CPU from the last decade;
//            ~20 cycles to read, sub-nanosecond resolution.
//  AArch64 : isb + cntvct_el0 (generic timer). On Apple Silicon this ticks at
//            24 MHz, i.e. ~41.7 ns per tick, which quantizes single-message
//            timings. Use batch timing on a Mac and per-message timing on
//            Linux x86 when you want real tail numbers.
//  other   : steady_clock fallback.
class CycleClock {
public:
    [[nodiscard]] static std::uint64_t now() noexcept {
#if defined(__x86_64__)
        __builtin_ia32_lfence();
        return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
        std::uint64_t v;
        asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
        return v;
#else
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
#endif
    }

    // Nanoseconds per tick, measured against steady_clock.
    [[nodiscard]] static double calibrate(std::chrono::milliseconds window = std::chrono::milliseconds{100}) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::uint64_t c0 = now();
        std::this_thread::sleep_for(window);
        const std::uint64_t c1 = now();
        const auto t1 = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        return static_cast<double>(ns) / static_cast<double>(c1 - c0);
    }
};

}  // namespace limitbook
