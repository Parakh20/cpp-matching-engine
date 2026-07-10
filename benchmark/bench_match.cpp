// Prefill N resting asks, then submit N aggressive bids that each fully
// match one resting ask; ns/match. Only the matching-submission phase is
// timed (prefill happens outside the timer).
#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "arena.hpp"
#include "engine.hpp"
#include "order.hpp"
#include "order_book.hpp"

using namespace me;

namespace {

void BM_Match(benchmark::State& state, BookKind kind, bool use_arena) {
    const int64_t n = state.range(0);
    for (auto _ : state) {
        state.PauseTiming();
        Engine engine(kind, /*min_price=*/0, /*tick=*/1);
        Arena arena;
        auto alloc = [&]() { return use_arena ? arena.alloc_order() : new Order(); };

        std::vector<Order*> resting;
        resting.reserve(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            Order* ask = alloc();
            ask->order_id = static_cast<uint64_t>(i + 1);
            ask->timestamp_ns = ask->order_id;
            ask->side = SIDE_ASK;
            ask->type = TYPE_LIMIT;
            ask->price = 10000 + (i % 500);
            ask->quantity = 1;
            engine.book().add_limit_order(ask);
            resting.push_back(ask);
        }

        std::vector<Order*> takers;
        takers.reserve(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            Order* bid = alloc();
            bid->order_id = static_cast<uint64_t>(n + i + 1);
            bid->timestamp_ns = bid->order_id;
            bid->side = SIDE_BUY;
            bid->type = TYPE_LIMIT;
            bid->price = 10000 + (i % 500);  // crosses the matching resting ask exactly
            bid->quantity = 1;
            takers.push_back(bid);
        }
        state.ResumeTiming();

        for (Order* bid : takers) {
            engine.book().add_limit_order(bid);
        }

        state.PauseTiming();
        for (Order* o : resting) { if (use_arena) arena.free_order(o); else delete o; }
        for (Order* o : takers) { if (use_arena) arena.free_order(o); else delete o; }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n);
}

}  // namespace

BENCHMARK_CAPTURE(BM_Match, V1_Naive, BookKind::Naive, false)
    ->Arg(1000)->Arg(10000)->Arg(100000)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Match, V2_Optimized_Heap, BookKind::Optimized, false)
    ->Arg(1000)->Arg(10000)->Arg(100000)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Match, V3_Optimized_Arena, BookKind::OptimizedArena, true)
    ->Arg(1000)->Arg(10000)->Arg(100000)->Unit(benchmark::kMicrosecond);
