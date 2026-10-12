// Proves the steady-state hot path performs zero heap allocations.
//
// This binary replaces the global allocation functions with counting
// versions. Each test warms up first (books created, vectors at their
// reserved capacity), then counts allocations while replaying a pre-encoded
// stream. Any allocation in decode -> book update -> index is a failure.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <vector>

#include "limitbook/itch/book_builder.hpp"
#include "limitbook/itch/decoder.hpp"
#include "limitbook/itch/encoder.hpp"
#include "limitbook/matching_engine.hpp"
#include "limitbook/sim/synthetic_feed.hpp"

namespace {
std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_allocations{0};

void* counted_alloc(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) {
        return p;
    }
    throw std::bad_alloc{};
}

struct CountScope {
    CountScope() {
        g_allocations = 0;
        g_counting = true;
    }
    CountScope(const CountScope&) = delete;
    CountScope& operator=(const CountScope&) = delete;
    CountScope(CountScope&&) = delete;
    CountScope& operator=(CountScope&&) = delete;
    ~CountScope() { g_counting = false; }
    [[nodiscard]] std::size_t count() const { return g_allocations.load(); }
};
}  // namespace

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace itch = limitbook::itch;

TEST(ZeroAlloc, CounterDetectsAllocations) {
    std::size_t n = 0;
    {
        CountScope scope;
        auto p = std::make_unique<std::vector<int>>(100);
        n = scope.count();
    }
    EXPECT_GE(n, 1U);  // sanity: the harness actually works
}

TEST(ZeroAlloc, ItchReplayAfterWarmup) {
    limitbook::sim::SyntheticFeed::Config fcfg;
    fcfg.symbols = 4;
    fcfg.seed = 7;
    limitbook::sim::SyntheticFeed feed{fcfg};

    std::vector<std::byte> warmup;
    std::vector<std::byte> measured;
    {
        itch::Encoder enc{warmup};
        feed.emit_directory(enc);
        feed.emit(enc, 200'000);
    }
    {
        itch::Encoder enc{measured};
        feed.emit(enc, 500'000);  // encoded before counting starts
    }

    itch::BookBuilder::Config cfg;
    cfg.max_orders = 1 << 16;
    cfg.max_levels = 1 << 14;
    cfg.level_reserve = 512;  // > max levels a synthetic book reaches
    auto builder = std::make_unique<itch::BookBuilder>(cfg);
    (void)itch::for_each_framed(std::span<const std::byte>{warmup}, *builder);

    std::size_t allocations = 0;
    itch::FrameStats stats;
    {
        CountScope scope;
        stats = itch::for_each_framed(std::span<const std::byte>{measured}, *builder);
        allocations = scope.count();
    }
    EXPECT_EQ(stats.errors, 0U);
    EXPECT_EQ(stats.messages, 500'000U);
    EXPECT_EQ(builder->stats().capacity_failures, 0U);
    EXPECT_EQ(allocations, 0U) << "hot path allocated " << allocations << " times";
}

namespace {
struct CountingSink {
    std::uint64_t fills{0};
    std::uint64_t qty{0};
    void on_fill(const limitbook::Fill& f) {
        ++fills;
        qty += f.qty;
    }
};
}  // namespace

TEST(ZeroAlloc, MatchingEngineSteadyState) {
    using Engine = limitbook::MatchingEngine<CountingSink>;
    CountingSink sink;
    Engine::Config cfg;
    cfg.max_orders = 1 << 14;
    cfg.max_levels = 1 << 12;
    cfg.level_reserve = 256;
    auto engine = std::make_unique<Engine>(cfg, sink);
    limitbook::sim::Rng rng{1};

    auto run = [&](limitbook::OrderId first, int n) {
        for (int i = 0; i < n; ++i) {
            const auto id = first + static_cast<limitbook::OrderId>(i);
            const auto side = rng.chance(50) ? limitbook::Side::Buy : limitbook::Side::Sell;
            const auto price = 1000 + static_cast<limitbook::Price>(rng.below(64));
            (void)engine->submit_limit(id, side, price, static_cast<limitbook::Qty>(1 + rng.below(100)));
            if (rng.chance(40) && id > 10) {
                (void)engine->cancel(id - rng.below(10));
            }
        }
    };
    run(1, 50'000);  // warmup

    std::size_t allocations = 0;
    {
        CountScope scope;
        run(1'000'000, 200'000);
        allocations = scope.count();
    }
    EXPECT_GT(sink.fills, 0U);
    EXPECT_EQ(allocations, 0U);
}
