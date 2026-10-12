#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace limitbook {

// Log-linear histogram (HdrHistogram-style) with 16 sub-buckets per power of
// two: relative error <= 6.25% from 1 ns to 2^64 ns, in a fixed 7.8 KB array.
// record() is branch-light, allocation-free, and safe to call per message.
//
// Averages hide the tail; this exists so every benchmark reports p99/p99.9.
class LatencyHistogram {
public:
    static constexpr int kSubBits = 4;
    static constexpr std::size_t kSub = std::size_t{1} << kSubBits;
    static constexpr std::size_t kBuckets = (64 - kSubBits + 1) * kSub;

    [[nodiscard]] static constexpr std::size_t bucket_index(std::uint64_t v) noexcept {
        if (v < kSub) {
            return static_cast<std::size_t>(v);
        }
        const int msb = 63 - std::countl_zero(v);
        const int shift = msb - kSubBits;
        const auto sub = static_cast<std::size_t>((v >> shift) & (kSub - 1));
        return static_cast<std::size_t>(msb - kSubBits + 1) * kSub + sub;
    }

    [[nodiscard]] static constexpr std::uint64_t bucket_lower_bound(std::size_t idx) noexcept {
        if (idx < kSub) {
            return idx;
        }
        const std::size_t octave = idx / kSub;
        const std::size_t sub = idx % kSub;
        const auto msb = static_cast<int>(octave) + kSubBits - 1;
        const int shift = msb - kSubBits;
        return (std::uint64_t{1} << msb) | (static_cast<std::uint64_t>(sub) << shift);
    }

    void record(std::uint64_t value) noexcept {
        ++counts_[bucket_index(value)];
        ++total_;
        sum_ += value;
        min_ = std::min(min_, value);
        max_ = std::max(max_, value);
    }

    // Upper edge of the bucket holding the p-th percentile (p in [0, 100]).
    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        if (total_ == 0) {
            return 0;
        }
        const auto rank = static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(total_ - 1)) + 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank) {
                const std::uint64_t upper =
                    (i + 1 < kBuckets) ? bucket_lower_bound(i + 1) - 1 : std::numeric_limits<std::uint64_t>::max();
                return std::min(upper, max_);
            }
        }
        return max_;
    }

    void merge(const LatencyHistogram& other) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) {
            counts_[i] += other.counts_[i];
        }
        total_ += other.total_;
        sum_ += other.sum_;
        min_ = std::min(min_, other.min_);
        max_ = std::max(max_, other.max_);
    }

    void reset() noexcept { *this = LatencyHistogram{}; }

    [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return total_ == 0 ? 0 : min_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept {
        return total_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(total_);
    }

private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t total_{0};
    std::uint64_t sum_{0};
    std::uint64_t min_{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t max_{0};
};

}  // namespace limitbook
