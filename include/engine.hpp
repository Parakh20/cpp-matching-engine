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

    // Submit a Limit, Market, Stop, or Stop-Limit order.
    //
    // Limit/Market orders go straight to the book. Stop/Stop-Limit orders never
    // touch the live book -- they rest dormant (FIFO per side, by placement
    // order) until a trade prints at a price that crosses their stop_price:
    // buy stops trigger when trade price >= stop_price, sell stops trigger
    // when trade price <= stop_price. On trigger, TYPE_STOP converts to
    // TYPE_MARKET and TYPE_STOP_LIMIT converts to TYPE_LIMIT (keeping its
    // original price/tif), then it is dispatched to the book like any other
    // order -- whose own resulting trade can trigger further stops, handled
    // as a loop until quiescent (cascading triggers).
    //
    // Returns every trade produced, including from cascaded triggers, in the
    // order they occurred.
    std::vector<Trade> submit_order(Order* order);

private:
    std::vector<Trade> dispatch_to_book(Order* order);
    void trigger_ready_stops(int64_t last_trade_price, std::vector<Trade>& out_trades);
    Order* pop_triggerable_buy_stop(int64_t last_trade_price);
    Order* pop_triggerable_sell_stop(int64_t last_trade_price);

    std::unique_ptr<IOrderBook> book_;
    uint64_t order_id_ = 0;
    uint64_t clock_ns_ = 0;
    std::vector<Order*> dormant_buy_stops_;   // FIFO by placement order
    std::vector<Order*> dormant_sell_stops_;  // FIFO by placement order
};

}  // namespace me

#endif  // MATCHING_ENGINE_ENGINE_HPP
