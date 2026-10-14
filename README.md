# C++ Matching Engine

[![CI](https://github.com/Parakh20/cpp-matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Parakh20/cpp-matching-engine/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A C++17 limit-order-book matching engine built in three versions, with the
optimization steps written up:

- **V1 naive**: `std::map` + `std::queue`
- **V2 optimized**: array-indexed price levels + intrusive doubly-linked list
- **V3 arena**: V2 + a bump-pointer allocator for `Order` objects

All three implement the same `IOrderBook` interface and run against the same
GoogleTest suite, so the optimized versions are checked to behave exactly like
the naive one.

Supported features: limit and market orders, cancel and modify, price-time
priority, time-in-force (GTC / IOC / FOK), Stop and Stop-Limit orders
(including cascading triggers), and self-trade prevention (cancel-resting).

## Quick start

Requirements: CMake 3.16+, a C++17 compiler (GCC or Clang), and network access
on the first configure. GoogleTest v1.15.2 and Google Benchmark v1.9.1 are
downloaded by CMake `FetchContent`.

```bash
git clone https://github.com/Parakh20/cpp-matching-engine.git
cd cpp-matching-engine
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The library is compiled with `-Wall -Wextra` and builds without warnings
(checked with GCC 13.3).

## Tests

```bash
ctest --test-dir build            # or: ./build/matching_engine_tests
```

There are 336 GoogleTest cases. Each `TEST_P` body (matching, price/time
priority, edge cases, order types, self-trade prevention) is parameterized
over all three engine versions through a shared fixture in
`tests/test_helpers.hpp`. The 336 is 28 test bodies x 4 suite instantiations
x 3 engines: every `INSTANTIATE_TEST_SUITE_P` call applies to all `TEST_P`
bodies of the shared fixture. Every case passes.

Line coverage from an earlier `gcov` run (Debug + `--coverage`, taken before
the order-type extensions): `src/naive/order_book.cpp` 93.6%,
`src/optimized/order_book.cpp` 92.0%, `src/engine.cpp` 85.7%.

## Benchmarks

```bash
./build/matching_engine_benchmarks                                   # full sweep, N = 1k / 10k / 100k
./build/matching_engine_benchmarks --benchmark_filter='/100000$'     # N = 100k only
./build/matching_engine_benchmarks --benchmark_format=json > results/benchmark_results.json
```

Results recorded on 2026-07-06 on a 13th Gen Intel Core i5-13500H (16 logical
CPUs), Release build, benchmarks compiled with `-O2 -DNDEBUG`. The full sweep is
in [`results/benchmark_results.json`](results/benchmark_results.json) and the
reasoning is in [`optimization_log.md`](optimization_log.md).

| N = 100,000 | V1 (ns/op) | V2 (ns/op) | V3 (ns/op) | V1 to V3 speedup |
|---|---|---|---|---|
| insert | 112.2 | 46.7 | 33.1 | 3.39x |
| match  | 125.6 | 73.8 | 50.6 | 2.48x |
| cancel | 41.5  | 25.1 | 20.2 | 2.06x |

These figures are mean CPU time per operation, not tail latency. Results
depend on the machine and its load.

## Project structure

```
include/              public headers
  order.hpp, trade.hpp  POD order/trade structs (prices are int64 ticks, no floating point)
  order_book.hpp        abstract IOrderBook interface
  engine.hpp            make_order_book() factory + Engine facade (ids, timestamps, stop orders)
  arena.hpp             bump-pointer Order allocator used by V3
src/
  naive/order_book.cpp      V1: std::map<price, std::queue>, lazy tombstone cancel
  optimized/order_book.cpp  V2/V3: vector-indexed levels, intrusive list, O(1) cancel
  engine.cpp                factory, Engine::submit_order, dormant stop-order handling
tests/                GoogleTest suite, parameterized over V1/V2/V3
benchmark/            Google Benchmark: insert, match, cancel
results/              recorded benchmark JSON
docs/design/          design spec for the extended order types
docs/plans/           original implementation plans (design notes)
optimization_log.md   V1 to V2 to V3 write-up with numbers
```

## Design notes

- **Prices** are `int64_t` ticks (dollars x 100). There is no floating point
  in the matching path.
- **V1** keeps a `std::map<price, std::queue<Slot>>` per side. Price lookup is
  O(log n). Cancels are lazy (epoch/tombstone) because `std::queue` cannot
  erase from the middle.
- **V2/V3** use a `std::vector<Level>` indexed by
  `(price - min_price) / tick`, so price lookup is O(1). Each level is an
  intrusive doubly-linked list of `Order*`, which allows an O(1) cancel from
  any position. Cached best-bid and best-ask indices give O(1) top-of-book
  reads.
- **V2 and V3 share one order-book class.** The difference is only who
  allocates `Order` objects: `new`/`delete` for V2, the `Arena` for V3. The
  book never allocates or frees orders itself.
- **Time-in-force**: GTC rests, IOC matches and then discards the remainder,
  and FOK runs a non-mutating `can_fill_completely` liquidity scan first. The
  scan skips the same trader's resting quantity, so self-trade prevention
  cannot turn a FOK order into a partial fill.
- **Self-trade prevention** uses `trader_id`, where `0` means the order does
  not participate.
- **Stop / Stop-Limit** orders never enter the book. `Engine::submit_order`
  holds them in a FIFO per side until a trade price crosses `stop_price`, then
  converts them to Market or Limit and re-submits them. This repeats until no
  more stops trigger, which handles cascades. Full design:
  [`docs/design/2026-07-10-extended-order-types-design.md`](docs/design/2026-07-10-extended-order-types-design.md).

## Limitations

- Single-threaded, in-memory, and single-instrument per book. There is no
  network or FIX gateway.
- Benchmarks report mean and CPU time, not p99. The arena's benefit to tail
  latency is probably larger than these numbers show.
- There is no Python binding.

## License

[MIT](LICENSE) © 2026 Parakh Sharma
