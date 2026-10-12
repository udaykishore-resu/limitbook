// limitbook-replay: rebuild every book from a NASDAQ ITCH 5.0 file and report
// per-message-type latency percentiles, throughput and invariant checks.
//
// Built with -fno-exceptions -fno-rtti: if any header on the hot path threw
// or used typeid/dynamic_cast, this target would not compile.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

#include "limitbook/cycle_clock.hpp"
#include "limitbook/itch/book_builder.hpp"
#include "limitbook/itch/decoder.hpp"
#include "limitbook/latency_histogram.hpp"

namespace {

using limitbook::CycleClock;
using limitbook::LatencyHistogram;
using limitbook::Side;
namespace itch = limitbook::itch;

struct Options {
    const char* path{nullptr};
    std::size_t max_orders{std::size_t{1} << 22};
    std::size_t max_levels{std::size_t{1} << 20};
    std::string_view symbol;
    std::size_t depth{5};
    bool latency{true};
    bool validate{false};
};

void usage() {
    std::fprintf(stderr,
                 "usage: limitbook-replay <itch-file> [options]\n"
                 "  --max-orders N   peak live orders across all symbols (default 4194304)\n"
                 "  --max-levels N   peak live price levels across all symbols (default 1048576)\n"
                 "  --symbol SYM     print final depth for SYM\n"
                 "  --depth K        levels to print per side (default 5)\n"
                 "  --validate       run invariant checks (execs at top, crossed book)\n"
                 "  --no-latency     skip per-message timing (pure throughput run)\n"
                 "\nThe file must be uncompressed: gunzip the NASDAQ .gz sample first.\n");
}

bool parse(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--max-orders") {
            const char* v = next();
            if (v == nullptr) return false;
            opt.max_orders = std::strtoull(v, nullptr, 10);
        } else if (arg == "--max-levels") {
            const char* v = next();
            if (v == nullptr) return false;
            opt.max_levels = std::strtoull(v, nullptr, 10);
        } else if (arg == "--symbol") {
            const char* v = next();
            if (v == nullptr) return false;
            opt.symbol = v;
        } else if (arg == "--depth") {
            const char* v = next();
            if (v == nullptr) return false;
            opt.depth = std::strtoull(v, nullptr, 10);
        } else if (arg == "--validate") {
            opt.validate = true;
        } else if (arg == "--no-latency") {
            opt.latency = false;
        } else if (!arg.empty() && arg[0] == '-') {
            return false;
        } else {
            opt.path = argv[i];
        }
    }
    return opt.path != nullptr;
}

struct MappedFile {
    const std::byte* data{nullptr};
    std::size_t size{0};
    int fd{-1};

    bool open(const char* path) {
        fd = ::open(path, O_RDONLY);
        if (fd < 0) {
            std::perror("open");
            return false;
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            std::perror("fstat");
            return false;
        }
        size = static_cast<std::size_t>(st.st_size);
        void* p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            std::perror("mmap");
            return false;
        }
        ::madvise(p, size, MADV_SEQUENTIAL);
        data = static_cast<const std::byte*>(p);
        return true;
    }

    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&&) = delete;
    MappedFile& operator=(MappedFile&&) = delete;
    ~MappedFile() {
        if (data != nullptr) {
            ::munmap(const_cast<std::byte*>(data), size);
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

constexpr std::array<char, 9> kTracked{'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P', 'R'};

std::size_t slot_of(char type) {
    for (std::size_t i = 0; i < kTracked.size(); ++i) {
        if (kTracked[i] == type) {
            return i;
        }
    }
    return kTracked.size();  // "other"
}

void print_price(std::int64_t p) {
    std::printf("%8lld.%04lld", static_cast<long long>(p / 10'000), static_cast<long long>(p % 10'000));
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        usage();
        return 2;
    }

    MappedFile file;
    if (!file.open(opt.path)) {
        return 1;
    }

    itch::BookBuilder::Config cfg;
    cfg.max_orders = opt.max_orders;
    cfg.max_levels = opt.max_levels;
    cfg.validate = opt.validate;
    auto* builder = new itch::BookBuilder(cfg);  // large object: keep it off the stack

    std::array<LatencyHistogram, kTracked.size() + 1> hist{};
    std::array<std::uint64_t, kTracked.size() + 1> counts{};
    std::uint64_t messages = 0;
    std::uint64_t errors = 0;

    const std::span<const std::byte> buf{file.data, file.size};
    const auto wall0 = std::chrono::steady_clock::now();
    std::size_t off = 0;
    while (off + 2 <= buf.size()) {
        const std::size_t len = limitbook::load_be<std::uint16_t>(buf.data() + off);
        off += 2;
        if (off + len > buf.size()) {
            std::fprintf(stderr, "warning: truncated message at offset %zu\n", off);
            break;
        }
        const auto msg = buf.subspan(off, len);
        off += len;
        ++messages;
        const std::size_t slot = slot_of(len > 0 ? static_cast<char>(msg[0]) : '\0');
        ++counts[slot];
        if (opt.latency) {
            const std::uint64_t t0 = CycleClock::now();
            const auto err = itch::decode(msg, *builder);
            const std::uint64_t t1 = CycleClock::now();
            hist[slot].record(t1 - t0);
            errors += err != itch::DecodeError::None ? 1U : 0U;
        } else {
            errors += itch::decode(msg, *builder) != itch::DecodeError::None ? 1U : 0U;
        }
    }
    const auto wall1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(wall1 - wall0).count();

    std::printf("file          %s (%.1f MB)\n", opt.path, static_cast<double>(file.size) / 1e6);
    std::printf("messages      %llu (%llu decode errors)\n", static_cast<unsigned long long>(messages),
                static_cast<unsigned long long>(errors));
    std::printf("wall time     %.3f s  ->  %.2f M msgs/s%s\n", secs, static_cast<double>(messages) / secs / 1e6,
                opt.latency ? "  (includes timer overhead)" : "");
    std::printf("books         %zu, live orders at end %zu\n", builder->book_count(), builder->live_orders());

    const auto& st = builder->stats();
    std::printf("adds %llu  execs %llu  cancels %llu  deletes %llu  replaces %llu\n",
                static_cast<unsigned long long>(st.adds), static_cast<unsigned long long>(st.executions),
                static_cast<unsigned long long>(st.cancels), static_cast<unsigned long long>(st.deletes),
                static_cast<unsigned long long>(st.replaces));
    std::printf("unknown refs %llu  capacity failures %llu%s\n", static_cast<unsigned long long>(st.unknown_refs),
                static_cast<unsigned long long>(st.capacity_failures),
                st.capacity_failures > 0 ? "  <-- raise --max-orders / --max-levels" : "");
    if (opt.validate) {
        std::printf("validation    executions off top-of-book %llu, crossed after add %llu\n",
                    static_cast<unsigned long long>(st.executions_off_top),
                    static_cast<unsigned long long>(st.crossed_after_add));
    }

    if (opt.latency) {
        const double ns_per_tick = CycleClock::calibrate();
        auto ns = [&](std::uint64_t ticks) { return static_cast<double>(ticks) * ns_per_tick; };
        std::printf("\nper-message decode+apply latency (ns), timer resolution %.2f ns/tick\n", ns_per_tick);
        std::printf("%-6s %12s %8s %8s %8s %8s %8s %10s\n", "type", "count", "p50", "p90", "p99", "p99.9",
                    "p99.99", "max");
        for (std::size_t i = 0; i < hist.size(); ++i) {
            if (counts[i] == 0) {
                continue;
            }
            const auto& h = hist[i];
            const char label = i < kTracked.size() ? kTracked[i] : '*';
            std::printf("%-6c %12llu %8.0f %8.0f %8.0f %8.0f %8.0f %10.0f\n", label,
                        static_cast<unsigned long long>(counts[i]), ns(h.percentile(50)), ns(h.percentile(90)),
                        ns(h.percentile(99)), ns(h.percentile(99.9)), ns(h.percentile(99.99)), ns(h.max()));
        }
    }

    if (!opt.symbol.empty()) {
        const auto locate = builder->locate_of(opt.symbol);
        if (!locate) {
            std::printf("\nsymbol %.*s not found\n", static_cast<int>(opt.symbol.size()), opt.symbol.data());
        } else {
            const auto* book = builder->book(*locate);
            std::printf("\n%.*s depth (locate %u)\n", static_cast<int>(opt.symbol.size()), opt.symbol.data(),
                        static_cast<unsigned>(*locate));
            std::printf("%28s | %-28s\n", "BID qty @ price (orders)", "ASK price x qty (orders)");
            std::array<limitbook::Quote, 64> bids{};
            std::array<limitbook::Quote, 64> asks{};
            std::size_t nb = 0;
            std::size_t na = 0;
            const std::size_t depth = opt.depth < bids.size() ? opt.depth : bids.size();
            book->for_each_level(Side::Buy, depth, [&](const limitbook::PriceLevel& l) {
                bids[nb++] = {l.price, l.total_qty, l.order_count()};
            });
            book->for_each_level(Side::Sell, depth, [&](const limitbook::PriceLevel& l) {
                asks[na++] = {l.price, l.total_qty, l.order_count()};
            });
            for (std::size_t i = 0; i < depth && (i < nb || i < na); ++i) {
                if (i < nb) {
                    std::printf("%9llu @ ", static_cast<unsigned long long>(bids[i].qty));
                    print_price(bids[i].price);
                    std::printf(" (%3u)", bids[i].orders);
                } else {
                    std::printf("%28s", "");
                }
                std::printf(" | ");
                if (i < na) {
                    print_price(asks[i].price);
                    std::printf(" x %-9llu (%3u)", static_cast<unsigned long long>(asks[i].qty), asks[i].orders);
                }
                std::printf("\n");
            }
        }
    }

    delete builder;
    return 0;
}
