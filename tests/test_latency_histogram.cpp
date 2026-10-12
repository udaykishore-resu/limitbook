#include <gtest/gtest.h>

#include <cstdint>

#include "limitbook/latency_histogram.hpp"

using limitbook::LatencyHistogram;

TEST(LatencyHistogram, BucketBoundsAreConsistent) {
    for (std::uint64_t v : {0ULL, 1ULL, 15ULL, 16ULL, 17ULL, 31ULL, 32ULL, 33ULL, 1000ULL, 123'456'789ULL,
                            (1ULL << 63) + 12345ULL}) {
        const auto idx = LatencyHistogram::bucket_index(v);
        ASSERT_LT(idx, LatencyHistogram::kBuckets);
        EXPECT_LE(LatencyHistogram::bucket_lower_bound(idx), v) << v;
        if (idx + 1 < LatencyHistogram::kBuckets) {
            EXPECT_GT(LatencyHistogram::bucket_lower_bound(idx + 1), v) << v;
        }
    }
    static_assert(LatencyHistogram::bucket_index(16) == 16);
    static_assert(LatencyHistogram::bucket_lower_bound(32) == 32);
}

TEST(LatencyHistogram, PercentilesWithinRelativeError) {
    LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 100'000; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.count(), 100'000U);
    EXPECT_EQ(h.min(), 1U);
    EXPECT_EQ(h.max(), 100'000U);
    EXPECT_NEAR(h.mean(), 50'000.5, 1e-9);
    for (double p : {50.0, 90.0, 99.0, 99.9}) {
        const double exact = p / 100.0 * 100'000.0;
        const auto got = static_cast<double>(h.percentile(p));
        EXPECT_GE(got, exact * 0.99) << p;
        EXPECT_LE(got, exact * 1.07) << p;  // 1/16 bucket width
    }
    EXPECT_EQ(h.percentile(100), 100'000U);
}

TEST(LatencyHistogram, MergeAndReset) {
    LatencyHistogram a, b;
    a.record(10);
    b.record(1000);
    a.merge(b);
    EXPECT_EQ(a.count(), 2U);
    EXPECT_EQ(a.max(), 1000U);
    a.reset();
    EXPECT_EQ(a.count(), 0U);
    EXPECT_EQ(a.percentile(99), 0U);
}
