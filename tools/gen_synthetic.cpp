// limitbook-gen-synthetic: write a deterministic ITCH 5.0 file (same framing
// as NASDAQ's binary files) so the replay tool can run without the 6+ GB
// sample. Same seed => byte-identical file on every platform.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "limitbook/itch/encoder.hpp"
#include "limitbook/sim/synthetic_feed.hpp"

int main(int argc, char** argv) {
    const char* out = "synthetic.itch";
    std::size_t messages = 5'000'000;
    limitbook::sim::SyntheticFeed::Config cfg;

    for (int i = 1; i < argc; i += 2) {
        const std::string_view arg{argv[i]};
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (v == nullptr) {
            std::fprintf(stderr, "missing value for %s\n", argv[i]);
            return 2;
        }
        if (arg == "--out") {
            out = v;
        } else if (arg == "--messages") {
            messages = std::strtoull(v, nullptr, 10);
        } else if (arg == "--symbols") {
            cfg.symbols = static_cast<std::uint16_t>(std::strtoul(v, nullptr, 10));
        } else if (arg == "--seed") {
            cfg.seed = std::strtoull(v, nullptr, 10);
        } else {
            std::fprintf(stderr,
                         "usage: limitbook-gen-synthetic [--out FILE] [--messages N] [--symbols K] [--seed S]\n");
            return 2;
        }
    }

    std::FILE* f = std::fopen(out, "wb");
    if (f == nullptr) {
        std::perror("fopen");
        return 1;
    }

    limitbook::sim::SyntheticFeed feed{cfg};
    std::vector<std::byte> buf;
    buf.reserve(8U << 20);
    limitbook::itch::Encoder enc{buf};
    feed.emit_directory(enc);

    constexpr std::size_t kChunk = 200'000;
    std::size_t written = 0;
    while (written < messages) {
        const std::size_t n = messages - written < kChunk ? messages - written : kChunk;
        feed.emit(enc, n);
        written += n;
        if (std::fwrite(buf.data(), 1, buf.size(), f) != buf.size()) {
            std::perror("fwrite");
            std::fclose(f);
            return 1;
        }
        buf.clear();
    }
    std::fclose(f);
    std::printf("wrote %zu order-flow messages for %u symbols to %s\n", messages,
                static_cast<unsigned>(cfg.symbols), out);
    return 0;
}
