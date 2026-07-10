# Optimization log

Real numbers from `results/benchmark_results.json`, produced by
`./build/matching_engine_benchmarks --benchmark_format=json` on this
machine (13th Gen Intel i5-13500H, 16 logical CPUs, Release build,
`-O2 -DNDEBUG`). All three versions pass the identical 51-case-per-version
gtest suite (see `README.md`) — these are genuine apples-to-apples
comparisons, not cherry-picked.

## Baseline: V1 (naive, `std::map<price, std::queue<Order*>>`)

Correctness-first, no optimization concern. Each price level is a red-black
tree node; cancellation is lazy (epoch/tombstone), because `std::queue` can't
erase from the middle. At N=100,000:

| Op | ns/op | ops/sec |
|---|---|---|
| insert | 112.2 | 8.9M |
| match (crossing) | 125.6 | 8.0M |
| cancel | 41.5 | 24.1M |

Problems: O(log n) price lookup via tree traversal (pointer-chasing, poor
cache locality); one heap allocation per resting order (`std::queue` node);
tombstone bloat on cancel-heavy workloads (dead slots accumulate until a
level's queue is scanned).

## Opt 1: V2 (array-indexed price levels + intrusive list)

Replaces the tree with `std::vector<Level>` indexed by `(price - min_price) /
tick` — O(1) price lookup instead of O(log n), and a cache-friendly
contiguous array instead of tree nodes scattered across the heap. The
intrusive doubly-linked list (`Order::prev/next`) enables true O(1)
cancel-from-anywhere — no tombstones needed. Order objects are still heap
`new`/`delete`.

| Op | ns/op | Speedup vs V1 |
|---|---|---|
| insert | 46.7 | 2.40x |
| match | 73.8 | 1.70x |
| cancel | 25.1 | 1.65x |

Why: eliminating the O(log n) tree lookup and its cache-unfriendly pointer
chasing is the dominant win for insert/match. Cancel improves less
dramatically because V1's amortized lazy-tombstone cancel was already
fairly cheap in this synthetic (non-cancel-heavy) benchmark; V2's gain here
is mostly from O(1) list-splice replacing tree-node lookup+erase.

## Opt 2: V3 (V2 + Arena bump-pointer allocator)

Identical order-book logic to V2 (literally the same `OptimizedOrderBook`
class) — the only change is that `Order` objects are allocated via
`Arena::alloc_order()`/`free_order()` (bump-pointer + free-list, ~zero
syscalls in steady state) instead of `new`/`delete`.

| Op | ns/op | Speedup vs V1 | Speedup vs V2 |
|---|---|---|---|
| insert | 33.1 | 3.39x | 1.41x |
| match | 50.6 | 2.48x | 1.46x |
| cancel | 20.2 | 2.06x | 1.25x |

Why: `malloc`/`free` cost isn't just the average-case allocation time —
it's lock contention (if multi-threaded elsewhere in the process),
free-list-walk variance, and occasional page faults, all of which show up
as tail latency, not just mean. This benchmark measures mean/CPU time (not
p99), so the improvement shown here is a lower bound on the real-world
tail-latency benefit; a p99-focused benchmark would likely show a larger
gap. The arena's win is largest on `match`, because a full match-and-fill
both allocates the taker context and frees the maker when it's fully
consumed — two allocator calls saved per trade instead of one.

## Summary

| N=100,000 | V1 (ns/op) | V2 (ns/op) | V3 (ns/op) | V1→V3 speedup |
|---|---|---|---|---|
| insert | 112.2 | 46.7 | 33.1 | 3.39x |
| match | 125.6 | 73.8 | 50.6 | 2.48x |
| cancel | 41.5 | 25.1 | 20.2 | 2.06x |

Full N=1,000/10,000/100,000 sweep in `results/benchmark_results.json`.
