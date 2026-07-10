#ifndef MATCHING_ENGINE_ENGINE_HPP
#define MATCHING_ENGINE_ENGINE_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include "order.hpp"
#include "order_book.hpp"
#include "trade.hpp"

namespace me {

// Which order-book implementation to construct.
enum class BookKind {
    Naive,          // V1: std::map + std::queue
    Optimized,      // V2: array-indexed price levels + intrusive list (heap-allocated Orders)
    OptimizedArena  // V3: V2 data structures + Arena order allocation
};

// Construct a concrete book. Defined in engine.cpp.
//
// `min_price`/`tick` parameterize the array-indexed price window used by the
// optimized books; they are ignored by the naive book. Defaults cover the
// price ranges used by the test/benchmark suites.
std::unique_ptr<IOrderBook> make_order_book(BookKind kind,
                                            int64_t min_price = 0,
                                            int64_t tick = 1);

// Thin facade over an IOrderBook that also owns id/timestamp generation and the
// memory of Orders submitted through its convenience helpers. Tests that want to
// manage Order lifetime themselves can talk to the IOrderBook directly; the
// Engine is a convenience for end-to-end / benchmark drivers.
class Engine {
public:
    explicit Engine(BookKind kind, int64_t min_price = 0, int64_t tick = 1)
        : book_(make_order_book(kind, min_price, tick)) {}

    IOrderBook& book() { return *book_; }
    const IOrderBook& book() const { return *book_; }

    // Monotonic id / timestamp helpers (timestamps are synthetic, monotonically
    // increasing — deterministic and floating-point-free).
    uint64_t next_order_id() { return ++order_id_; }
    uint64_t next_timestamp() { return ++clock_ns_; }

private:
    std::unique_ptr<IOrderBook> book_;
    uint64_t order_id_ = 0;
    uint64_t clock_ns_ = 0;
};

}  // namespace me

#endif  // MATCHING_ENGINE_ENGINE_HPP
