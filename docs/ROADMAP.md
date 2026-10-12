# Roadmap

Milestone 1 is done: a correct, tested, allocation-free core. Every later milestone has a **deliverable you can show** and a **question it must answer with data**.

## M2: A real NASDAQ day
- Download a full-day sample (see `data/README.md`), replay with `--validate`.
- Record peak live orders and levels, and size `Config` from them (print them at the end of replay).
- Publish per-message p50/p99/p99.9 on **Linux x86** (`rdtsc`), next to Apple Silicon throughput.
- **Answer:** does the 1.7x gap over the `std::map` oracle widen on real depth? Why or why not? (Use `perf stat -e cache-misses`.)

## M3: Level search, measured
- Implement two alternatives behind the same `BookSide` interface: binary search, and a dense ladder (array indexed by `(price - base) / tick` with recentering).
- Benchmark all three on a real-day replay and on adversarial far-from-touch flow.
- **Answer:** at what book depth and update distribution does each one win?

## M4: Profile-driven optimization
- `perf record -g` plus a flamegraph of the replay. On macOS use Instruments → Time Profiler.
- Candidates: hot/cold field split in `Order`, prefetching the index slot, `__builtin_expect` audits, huge pages for the pools.
- **Answer:** which two changes moved p99, and by how much? Commit before/after histograms.

## M5: Independent BBO validation
- Compare the reconstructed best bid/offer at each trade timestamp against a vendor TAQ/NBBO file for one symbol.
- **Answer:** what is the residual mismatch, and is it explained by hidden/midpoint liquidity?

## M6: Matching engine features
- Self-trade prevention (cancel-newest / cancel-oldest / decrement).
- Iceberg orders (display qty, replenish at the back of the queue).
- Opening auction: uncross at the price that maximizes executed volume.

## M7: Live pipeline with feedline
- Add `feedline` as a dependency (`FetchContent` from your GitHub repo or `add_subdirectory(../feedline)`).
- Consumer thread: `SpscRing<Slot>` → `itch::decode(slot, BookBuilder&)`.
- **Answer:** end-to-end wire-to-book latency distribution with 1% loss on one line.
