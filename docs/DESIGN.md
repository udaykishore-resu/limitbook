# limitbook design

## Goals and non-goals

**Goals:** a correct full-depth (L3) book for every NASDAQ instrument, rebuilt from ITCH 5.0. No heap allocation in steady state. Tail latency measured per message type. A price-time matching engine that reuses the same book.

**Non-goals (for now):** hidden or midpoint liquidity (not on the ITCH book), auction crossing logic, multi-threaded book updates. One instrument's updates are inherently serial, so the scaling unit is "shard symbols across cores," not "lock a book."

## Data path

```
mmap(file) → for_each_framed → decode(msg, handler) → BookBuilder::operator()(const AddOrder&) → OrderBook::add
```

1. **Framing.** NASDAQ binary files and MoldUDP64 packets both carry `u16 big-endian length | message`. `for_each_framed` walks that and never copies.
2. **Decoding.** Each message type is a *view* (`const std::byte*` plus accessors). Offsets are constants, so `AddOrder::price()` compiles to one unaligned load plus a `bswap`/`rev`. `static_assert` checks every view's size against the ITCH spec table.
3. **Dispatch.** `decode` switches on the type byte. It calls the handler only if the handler is invocable with that view type (`if constexpr (std::is_invocable_v<...>)`). A handler that ignores Trade messages pays for nothing except the length check.
4. **Book update.** `BookBuilder` maps the global order reference to `Order*` (`OrderIndex`) and the stock locate to `OrderBook*`.

## Memory model

All memory is sized from `Config` at construction:

| Pool | Element | Sized by |
|---|---|---|
| `ObjectPool<Order>` | 48 B (hook 16, id 8, price 8, qty 4, side 1, level ptr 8) | peak live orders across all symbols |
| `ObjectPool<PriceLevel>` | price, total qty, intrusive list head/tail/size | peak live levels across all symbols |
| `OrderIndex` | 16 B slot, power-of-two capacity at ≤ 7/8 load | peak live orders |
| per-book level vectors | 8 B pointer per level | `level_reserve` per side |

Exhaustion never throws and never allocates. The operation fails, `capacity_failures` increments, and the replay tool tells you which limit to raise. The single allowed post-warmup allocation is a level vector growing past `level_reserve`. The zero-allocation test sizes the reserve so that never happens, and Milestone 2 sizes it from a real day.

**Why pools are LIFO:** the most recently freed `Order` is the one most likely still in L1/L2, and add/delete churn near the touch is exactly that pattern.

## Book side: why a sorted vector

Order flow is concentrated within a few ticks of the best price. `BookSide` stores level pointers **worst → best**, so the best level is `back()`:

- **find/insert:** scan backwards from the best level. A handful of comparisons over contiguous memory for near-touch prices.
- **erase:** usually removes a level near the top, so only the few better levels shift.
- **best():** `back()`, O(1), no pointer chasing.

The pathological case is a far-from-touch price on a deep book (O(levels) scan and shift). Milestone 3 measures this against binary search and a dense ladder indexed by `(price - base) / tick` rather than arguing about it.

## Order index: open addressing with backward-shift deletion

- **Hash:** murmur3 `fmix64`. ITCH order refs are nearly sequential; an identity hash would pack them into one long probe run.
- **Empty slot marker:** `value == nullptr`, so every key including 0 is valid.
- **Deletion:** backward shift, not tombstones. Tombstones accumulate over a day of add/delete churn and lengthen every probe. Backward shift keeps probe lengths as they were at insert time. The invariant check is `home_in_gap = i < k <= j` (cyclically); an entry moves into the hole only if that wouldn't put it before its home slot.
- **No rehash:** `insert` returns false at 7/8 load. Rehashing a multi-million-entry table at 9:30:00.001 is not an option.

## ITCH semantics implemented

| Msg | Action |
|---|---|
| `R` Stock Directory | create the book and record the symbol (pre-market, so allocation happens before the open) |
| `A` / `F` Add | insert at the back of its price level (time priority) |
| `E` Executed | reduce by shares; remove at zero; `--validate` checks it was at the best price |
| `C` Executed with price | same as `E` (the price can differ from the order's price in crosses) |
| `X` Cancel | partial reduce; remove at zero |
| `D` Delete | remove |
| `U` Replace | remove the original, add the new ref on the same side at the new price/size (priority is lost) |
| others | length-checked and skipped |

**Validation counters.** `executions_off_top` and `crossed_after_add` are invariant checks, not ground truth. Both are legitimately non-zero around the opening and closing crosses. A true BBO check against an independent source is Milestone 5.

## Matching engine

- Single instrument, price-time priority, fills at the **maker's** price.
- `submit_limit` (GTC / IOC), `submit_market`, `cancel`, `reduce_to` (quantity down keeps priority, per exchange convention).
- Fill notification goes through a `FillSink` concept resolved at compile time. A sink must not re-enter the engine.
- The matching loop re-reads the best level after every fill, because a fill can destroy the level.

## Determinism

`sim::Rng` (xoshiro256**) replaces `std::mt19937` plus `std::uniform_int_distribution`. The standard *engines* are portable, but the *distributions* are implementation-defined: libstdc++ and libc++ produce different values from the same seed. A synthetic fixture that differs between a Mac and Linux CI is useless.
