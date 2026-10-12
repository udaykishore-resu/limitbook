// Benchmarks. Run from a Release build:
//   ./build/release/bench/limitbook_bench --benchmark_counters_tabular=true
//
// Each replay benchmark processes the same pre-encoded 1M-message synthetic
// stream, so items/s numbers are directly comparable across rows.

#include <benchmark/benchmark.h>

#include <memory>
#include <span>
#include <vector>

#include "limitbook/itch/book_builder.hpp"
#include "limitbook/itch/decoder.hpp"
#include "limitbook/itch/encoder.hpp"
#include "limitbook/matching_engine.hpp"
#include "limitbook/sim/reference_book.hpp"
#include "limitbook/sim/rng.hpp"
#include "limitbook/sim/synthetic_feed.hpp"

namespace itch = limitbook::itch;
using limitbook::Side;

namespace {

constexpr std::size_t kMessages = 1'000'000;

const std::vector<std::byte>& stream() {
    static const std::vector<std::byte> buf = [] {
        std::vector<std::byte> out;
        limitbook::sim::SyntheticFeed::Config cfg;
        cfg.symbols = 8;
        cfg.seed = 2024;
        limitbook::sim::SyntheticFeed feed{cfg};
        itch::Encoder enc{out};
        feed.emit_directory(enc);
        feed.emit(enc, kMessages);
        return out;
    }();
    return buf;
}

itch::BookBuilder::Config builder_config() {
    itch::BookBuilder::Config cfg;
    cfg.max_orders = 1 << 18;
    cfg.max_levels = 1 << 16;
    cfg.level_reserve = 256;
    return cfg;
}

// Decode only: the floor for any handler.
void BM_ItchDecodeOnly(benchmark::State& state) {
    const auto& buf = stream();
    for (auto _ : state) {
        std::uint64_t refs = 0;
        auto handler = [&](const itch::AddOrder& m) { refs += m.order_ref(); };
        auto stats = itch::for_each_framed(std::span<const std::byte>{buf}, handler);
        benchmark::DoNotOptimize(refs);
        benchmark::DoNotOptimize(stats.messages);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kMessages));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(buf.size()));
}
BENCHMARK(BM_ItchDecodeOnly)->Unit(benchmark::kMillisecond);

// Decode + full-depth book reconstruction with the pooled, intrusive book.
void BM_ItchReplayLimitbook(benchmark::State& state) {
    const auto& buf = stream();
    for (auto _ : state) {
        state.PauseTiming();
        auto builder = std::make_unique<itch::BookBuilder>(builder_config());
        state.ResumeTiming();
        (void)itch::for_each_framed(std::span<const std::byte>{buf}, *builder);
        auto live = builder->live_orders();
        benchmark::DoNotOptimize(live);
        state.PauseTiming();
        builder.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kMessages));
}
BENCHMARK(BM_ItchReplayLimitbook)->Unit(benchmark::kMillisecond);

// The same stream applied to the std::map / std::list / unordered_map oracle:
// the "textbook" order book this project exists to beat.
struct ReferenceHandler {
    std::vector<limitbook::sim::ReferenceBook> books{65536};

    void operator()(const itch::AddOrder& m) {
        books[m.stock_locate()].add(m.order_ref(), m.side(), m.price(), m.shares());
    }
    void operator()(const itch::AddOrderMpid& m) {
        books[m.stock_locate()].add(m.order_ref(), m.side(), m.price(), m.shares());
    }
    void operator()(const itch::OrderExecuted& m) { books[m.stock_locate()].reduce(m.order_ref(), m.executed_shares()); }
    void operator()(const itch::OrderCancel& m) { books[m.stock_locate()].reduce(m.order_ref(), m.cancelled_shares()); }
    void operator()(const itch::OrderDelete& m) { books[m.stock_locate()].remove(m.order_ref()); }
    void operator()(const itch::OrderReplace& m) {
        auto& b = books[m.stock_locate()];
        const auto old = b.get(m.original_order_ref());
        if (old) {
            b.remove(m.original_order_ref());
            b.add(m.new_order_ref(), old->side, m.price(), m.shares());
        }
    }
};

void BM_ItchReplayStdMapBaseline(benchmark::State& state) {
    const auto& buf = stream();
    for (auto _ : state) {
        state.PauseTiming();
        auto handler = std::make_unique<ReferenceHandler>();
        state.ResumeTiming();
        (void)itch::for_each_framed(std::span<const std::byte>{buf}, *handler);
        auto live = handler->books[1].size();
        benchmark::DoNotOptimize(live);
        state.PauseTiming();
        handler.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kMessages));
}
BENCHMARK(BM_ItchReplayStdMapBaseline)->Unit(benchmark::kMillisecond);

struct NullSink {
    std::uint64_t qty{0};
    void on_fill(const limitbook::Fill& f) { qty += f.qty; }
};

// Passive add + cancel near the touch: the dominant real-world pattern.
void BM_MatchingAddCancel(benchmark::State& state) {
    using Engine = limitbook::MatchingEngine<NullSink>;
    NullSink sink;
    Engine::Config cfg;
    cfg.max_orders = 1 << 16;
    auto eng = std::make_unique<Engine>(cfg, sink);
    for (limitbook::OrderId i = 0; i < 1000; ++i) {  // standing depth
        (void)eng->submit_limit(i, Side::Buy, 900 + static_cast<limitbook::Price>(i % 50), 100);
        (void)eng->submit_limit(100'000 + i, Side::Sell, 1100 + static_cast<limitbook::Price>(i % 50), 100);
    }
    limitbook::sim::Rng rng{3};
    limitbook::OrderId id = 1'000'000;
    for (auto _ : state) {
        const auto price = 940 + static_cast<limitbook::Price>(rng.below(10));
        (void)eng->submit_limit(id, Side::Buy, price, 100);
        (void)eng->cancel(id);
        ++id;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_MatchingAddCancel);

// Aggressive order sweeping `range(0)` resting orders, then replenish.
void BM_MatchingSweep(benchmark::State& state) {
    using Engine = limitbook::MatchingEngine<NullSink>;
    NullSink sink;
    Engine::Config cfg;
    cfg.max_orders = 1 << 16;
    auto eng = std::make_unique<Engine>(cfg, sink);
    const auto depth = static_cast<limitbook::Qty>(state.range(0));
    limitbook::OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        for (limitbook::Qty i = 0; i < depth; ++i) {
            (void)eng->submit_limit(id++, Side::Sell, 1000 + static_cast<limitbook::Price>(i % 8), 10);
        }
        state.ResumeTiming();
        auto result = eng->submit_market(id++, Side::Buy, depth * 10);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_MatchingSweep)->Arg(1)->Arg(16)->Arg(256);

}  // namespace
