# Implementation Plan: C++ Low-Latency Matching Engine

Date: 2026-06-24
Goal: Build a performance-obsessed limit-order-book matching engine end-to-end with a
documented optimization journey (V1 naive -> V2 array+intrusive-list -> V3 arena allocator),
proven correct by a single gtest suite that all three versions pass unchanged, with
Google Benchmark numbers recorded as JSON, plus a ctypes Python wrapper over a C ABI.

## Guiding constraints
- C++17 minimum (we compile with `-std=c++17`).
- Integer prices only: `int64_t price` = dollars * 100. NO floating point in the hot path.
- V2 and V3 MUST pass the EXACT SAME gtest suite as V1 (behavioral proof of equivalence).
- Tests built `-O0 -g`; benchmarks built `-O2 -DNDEBUG`.
- Commit working increments locally with conventional commits. Never push to origin.

## Toolchain bootstrap
- [x] Create dir tree (include/, src/naive, src/optimized, benchmark/, tests/, python_wrapper/, results/).
- [x] `python3 -m venv .venv && .venv/bin/pip install cmake` -> `.venv/bin/cmake`.
- [x] `.gitignore` for .venv/, build/, *.o, CMake artifacts, *.so.
- [x] Write this plan and commit it (required deliverable).

## Phase 1 - Public headers (the contract)
- [x] `include/order.hpp` - `Order` struct (ids, ts, side B/A, type L/M, int64 price/qty/filled, status, intrusive prev/next).
- [x] `include/trade.hpp` - `Trade` struct (ids, ts, price, qty, maker/taker ids, aggressor side).
- [x] `include/order_book.hpp` - abstract `IOrderBook` (add_limit_order, add_market_order, cancel_order, modify_order, best_bid, best_ask). Both impls inherit this so tests/benchmarks are polymorphic.
- [x] `include/engine.hpp` - thin `Engine` facade owning an `IOrderBook` + monotonic id/timestamp generation.

## Phase 2 - V1 naive engine (baseline, correctness first)
- [x] `src/naive/order_book.cpp` - `NaiveOrderBook`:
      `std::map<int64_t, std::queue<Order*>> bids/asks`, `std::unordered_map<uint64_t, Order*> order_index`.
      Price-time priority. Bids matched best (highest) first via `rbegin`, asks lowest first via `begin`.
      Trades emitted AT MAKER PRICE. Market orders walk levels until filled or book empty.
- [x] `src/engine.cpp` - shared Engine facade + a factory the tests/benches use.
- [x] Compile V1 standalone (`-O0 -g`) to shake out errors before tests.

## Phase 3 - Correctness tests (write BEFORE optimizing) - gtest via FetchContent
- [x] `tests/test_matching.cpp`: single bid x single ask at maker price; partial fill (one big eats several small);
      market order walks multiple levels; no-match rests on book.
- [x] `tests/test_priorities.cpp`: price priority (better price fills first); time priority (FIFO at same price);
      3-level sweep.
- [x] `tests/test_edge_cases.cpp`: cancel non-existent -> false; cancel filled -> false; market on empty -> 0 fills;
      crossed limit (bid > best ask) -> immediate match; modify reduce keeps priority; modify increase loses priority.
- [x] CMake `tests` target (links V1+V2+V3, gtest, `-O0 -g`), `enable_testing()` + `add_test`.
- [x] Run: V1 passes 100% green before any optimization work.

## Phase 4 - V1 benchmarks (Google Benchmark via FetchContent, BENCHMARK_ENABLE_TESTING=OFF)
- [x] `benchmark/bench_insert.cpp` - N random-price limit inserts into empty book; ns/op.
- [x] `benchmark/bench_match.cpp` - prefill N resting, submit aggressive full-match orders; ns/match.
- [x] `benchmark/bench_cancel.cpp` - insert N then cancel all; ns/cancel.
- [x] `benchmark/bench_e2e.cpp` - mix 60% limit / 30% cancel / 10% market, prices +/-2% of mid; ops/sec.
- [x] N in {1000, 10000, 100000} where sensible. `benchmarks` CMake target `-O2 -DNDEBUG`.
- [x] Run with `--benchmark_format=json`; curate into `results/v1_benchmark.json`.

## Phase 5 - V2 optimized engine (array price levels + intrusive list)
- [x] `src/optimized/order_book.cpp` - `OptimizedOrderBook`:
      `PRICE_LEVELS = 20000`, `PriceLevel bid_levels[]/ask_levels[]` (total_qty, head, tail),
      index = (price - min_price)/tick, track best_bid_idx/best_ask_idx, update on every mutation.
      O(1) price lookup vs map O(log n). Intrusive doubly-linked list (Order.prev/next) -> O(1) cancel + O(1) append.
- [x] Implements SAME `IOrderBook`. Run the SAME gtest suite -> all green.
- [x] Benchmark V2 -> `results/v2_benchmark.json`.

## Phase 6 - V3 arena allocator
- [x] `src/optimized/allocator.cpp` (+ `include/arena.hpp`) - `Arena`:
      `BLOCK_SIZE = 1<<20`, bump-pointer alloc from 1MB blocks, free list for reuse, no malloc/free in steady state.
- [x] V3 order book (Optimized wired to allocate Orders from Arena, selected at construction).
- [x] Run the SAME gtest suite -> all green.
- [x] Benchmark V3 -> `results/v3_benchmark.json`.

## Phase 7 - Results, log, docs
- [x] Aggregate `results/benchmark_results.json` (all three versions side by side).
- [x] `optimization_log.md` - Baseline V1 (numbers + problems), Opt1 V2 (numbers, Xx, why cache locality),
      Opt2 V3 (numbers, additional Xx, p99 variance, why malloc elimination), summary table + speedup row. REAL numbers.
- [x] `README.md` - what it is, build steps, V1/V2/V3 table, optimization journey, architecture rationale,
      how to run tests, python wrapper usage.

## Phase 8 - Python ctypes wrapper
- [x] C ABI in `src/c_api.cpp` (`extern "C"`: create_book/add_limit/add_market/cancel/free + trade out-params).
- [x] CMake `libmatchingengine` shared target (`-O2`).
- [x] `python_wrapper/engine_wrapper.py` - `ctypes.CDLL`, methods return trades as list of dicts.
- [x] Self-contained Python parity test exercising the .so directly.

## Phase 9 - Final verification
- [x] All three versions compile.
- [x] Full gtest suite green against V1, V2, V3.
- [x] All benchmarks run; JSON populated with real numbers.
- [x] optimization_log.md table + README populated with measured numbers.
- [x] Final commit.

## Risks / decisions
- No system cmake/gtest/gbench -> FetchContent pulls pinned googletest v1.15.2 + benchmark v1.9.4 over network.
- Array price-level scheme needs a price window; tests use small prices so a min_price + tick covers them.
  Engine clamps/guards out-of-window prices defensively (shouldn't occur for our inputs).
- Trades at maker price is the matching invariant the tests lock down; keep identical across versions.
- Arena: single-threaded steady-state reuse is the target; lock-free free list is OPTIONAL (note in README if done).
