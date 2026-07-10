# cpp matching engine

A performance-obsessed C++17 limit-order-book matching engine with a
documented optimization journey: **V1 naive** (`std::map` + `std::queue`) →
**V2 optimized** (array-indexed price levels + intrusive linked list) →
**V3 arena-allocated** (V2 + a bump-pointer allocator for `Order` objects).
All three versions implement the same `IOrderBook` interface and pass the
identical GoogleTest suite unchanged — the strongest available proof that
the optimizations are behavior-preserving, not just faster.

## Status

Buildable and tested, as of this session. `docs/superpowers/plans/` marks
every phase `[x]`, but that plan document does **not** reflect the actual
history of this repo — before this session, only the headers (`include/`)
and the V1 naive engine (`src/naive/order_book.cpp`) existed; there was no
build system, no V2/V3, no tests, and no benchmarks. Treat the plan doc as
aspirational design notes, not a changelog.

## Build & test

```
python3 -m venv .venv && .venv/bin/pip install cmake  # only needed for the cmake binary
source .venv/bin/activate
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/matching_engine_tests          # 336 tests (28 cases x 4 fixture instantiations x 3 engine versions), all passing
./build/matching_engine_benchmarks --benchmark_format=json > results/benchmark_results.json
```

GoogleTest and Google Benchmark are fetched automatically via CMake
`FetchContent` (requires network access on first configure).

## Results

336/336 gtest cases passing (28 distinct test bodies — matching, price/time
priority, edge cases, and order types/self-trade prevention, including
FOK+STP and Stop-order+STP interaction coverage added by a final
whole-branch review — each run against V1, V2, and V3). The 336 figure is
not simply 28 x 3: every one of the 4 `INSTANTIATE_TEST_SUITE_P`
registrations against the shared `OrderBookTest` fixture applies to *all*
28 `TEST_P` bodies across every test file, not just the ones registered in
the same file, so the real count is 28 bodies x 4 instantiations x 3
`BookKind` = 336 — a test-organization redundancy (no logical duplication,
every one of the 336 still independently passes), not a bug. Core-logic
line coverage (via `gcov`, Debug +
`--coverage` build, as of the pre-order-types baseline):
`src/naive/order_book.cpp` 93.6% (141 lines), `src/optimized/order_book.cpp`
92.0% (163 lines), `src/engine.cpp` 85.7% (7 lines).

Benchmark numbers (this machine: 13th Gen Intel i5-13500H, Release
`-O2 -DNDEBUG`; full sweep in `results/benchmark_results.json`, narrative in
`optimization_log.md`):

| N=100,000 | V1 (ns/op) | V2 (ns/op) | V3 (ns/op) | V1→V3 speedup |
|---|---|---|---|---|
| insert | 112.2 | 46.7 | 33.1 | 3.39x |
| match | 125.6 | 73.8 | 50.6 | 2.48x |
| cancel | 41.5 | 25.1 | 20.2 | 2.06x |

## Architecture

- `include/order.hpp`, `include/trade.hpp` — POD structs. Prices are
  `int64_t` ticks (dollars x 100) — no floating point anywhere in the
  matching path. `Order::prev/next` are intrusive doubly-linked-list links
  used by V2/V3 (V1 ignores them).
- `include/order_book.hpp` — the abstract `IOrderBook` interface
  (add_limit_order / add_market_order / cancel_order / modify_order /
  best_bid / best_ask) all three engines implement.
- `include/engine.hpp` + `src/engine.cpp` — `make_order_book(BookKind, ...)`
  factory and a thin `Engine` facade (id/timestamp generation).
- `src/naive/order_book.cpp` (V1) — `std::map<price, std::queue<Slot>>` per
  side, O(log n) price lookup, epoch/tombstone lazy cancellation (a
  `std::queue` can't erase from the middle).
- `src/optimized/order_book.cpp` (V2/V3 — literally the same class) —
  `std::vector<Level>` indexed by `(price - min_price) / tick`, O(1) price
  lookup; each level is an intrusive doubly-linked list of `Order*`, giving
  true O(1) cancel-from-anywhere with no tombstones. A cached
  `best_bid_idx_`/`best_ask_idx_` gives O(1) best-price reads in the common
  case, falling back to a bounded linear rescan only when the current best
  level empties out.
- `include/arena.hpp` — bump-pointer `Order` allocator (1 MiB blocks +
  free-list) used by V3. **V2 and V3 share the exact same order-book class**
  — the only difference is whether the *caller* allocates `Order` objects
  via `new`/`delete` (V2) or the `Arena` (V3); the book itself never
  allocates or frees `Order` memory (see the comment in `src/engine.cpp`).
- `tests/` — GoogleTest, `TEST_P` parameterized over all 3 `BookKind`
  values via a shared fixture (`tests/test_helpers.hpp`), so every test body
  runs against V1, V2, and V3 automatically.
- `benchmark/` — Google Benchmark, `BENCHMARK_CAPTURE`'d per engine version
  and allocator, at N in {1,000 / 10,000 / 100,000}.
- **Order types**: beyond plain Limit/Market, `Order` carries a
  `tif` (time-in-force: `TIF_GTC` default/existing-rest behavior, `TIF_IOC`
  match-then-discard, `TIF_FOK` fill-completely-or-not-at-all via a
  non-mutating `IOrderBook::can_fill_completely` liquidity scan that also
  excludes same-trader resting quantity so FOK can't partial-fill or trigger
  an unwanted self-trade-prevention cancellation on an otherwise-failed
  check — a bug the per-task reviews couldn't see since it spans the FOK
  and STP features independently, caught by the final whole-branch review),
  a
  `trader_id` for self-trade prevention (cancel-resting mode; `trader_id ==
  0` is the sentinel for "not participating," keeping every pre-existing
  order unaffected), and Stop/Stop-Limit support. Stop orders never enter
  V1/V2/V3's live book at all — `Engine::submit_order` holds them dormant
  (FIFO per side) until a trade price crosses their `stop_price`, then
  converts them to Market/Limit and re-dispatches, looping to handle
  cascading triggers. Design: `docs/superpowers/specs/2026-07-10-extended-order-types-design.md`.

## What's NOT done

- No Python `ctypes` C-ABI wrapper (`python_wrapper/`, Phase 8 of the plan
  doc) — not built in this session; the plan doc's checkbox for it is
  aspirational, not real.
- Benchmarks measure mean/CPU time, not p99/tail latency — the arena's
  real-world advantage (avoiding malloc lock contention and page-fault
  variance) is likely understated by a mean-only benchmark; see
  `optimization_log.md`.
