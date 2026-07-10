// Insert N resting orders, then cancel all N; ns/cancel. Only the
// cancellation phase is timed (insertion happens outside the timer).
#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "arena.hpp"
#include "engine.hpp"
#include "order.hpp"
#include "order_book.hpp"

using namespace me;

namespace {

void BM_Cancel(benchmark::State& state, BookKind kind, bool use_arena) {
    const int64_t n = state.range(0);
    for (auto _ : state) {
        state.PauseTiming();
        Engine engine(kind, /*min_price=*/0, /*tick=*/1);
        Arena arena;
        auto alloc = [&]() { return use_arena ? arena.alloc_order() : new Order(); };

        std::vector<Order*> owned;
        owned.reserve(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            Order* o = alloc();
            o->order_id = static_cast<uint64_t>(i + 1);
            o->timestamp_ns = o->order_id;
            o->side = (i % 2 == 0) ? SIDE_BUY : SIDE_ASK;
            o->type = TYPE_LIMIT;
            o->price = 10000 + (o->side == SIDE_BUY ? -(i % 500 + 1) : (i % 500 + 1));
            o->quantity = 1;
            engine.book().add_limit_order(o);
            owned.push_back(o);
        }
        state.ResumeTiming();

        for (Order* o : owned) {
            engine.book().cancel_order(o->order_id);
        }

        state.PauseTiming();
        for (Order* o : owned) { if (use_arena) arena.free_order(o); else delete o; }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n);
}

}  // namespace

BENCHMARK_CAPTURE(BM_Cancel, V1_Naive, BookKind::Naive, false)
    ->Arg(1000)->Arg(10000)->Arg(100000)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Cancel, V2_Optimized_Heap, BookKind::Optimized, false)
    ->Arg(1000)->Arg(10000)->Arg(100000)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Cancel, V3_Optimized_Arena, BookKind::OptimizedArena, true)
    ->Arg(1000)->Arg(10000)->Arg(100000)->Unit(benchmark::kMicrosecond);
