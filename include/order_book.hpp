#ifndef MATCHING_ENGINE_ORDER_BOOK_HPP
#define MATCHING_ENGINE_ORDER_BOOK_HPP

#include <cstdint>
#include <vector>

#include "order.hpp"
#include "trade.hpp"

namespace me {

// Sentinel returned by best_bid()/best_ask() when that side is empty.
constexpr int64_t NO_PRICE = -1;

// Abstract limit-order-book interface.
//
// Every concrete engine (NaiveOrderBook = V1, OptimizedOrderBook = V2/V3)
// implements this, so the gtest suite and the benchmarks run polymorphically
// over the implementation. The book takes ownership of the lifetime contract
// described per method; callers allocate `Order` objects and hand them in.
class IOrderBook {
public:
    virtual ~IOrderBook() = default;

    // Submit a limit order. The book matches it against the opposite side as
    // far as price allows; any unfilled remainder rests on the book. Returns the
    // trades generated (possibly empty). The book takes ownership of `order`
    // when (and only when) a remainder rests; fully-filled orders are not
    // retained. Trades print at the resting (maker) price.
    virtual std::vector<Trade> add_limit_order(Order* order) = 0;

    // Submit a market order. Matches against the opposite side until filled or
    // the book is exhausted. Never rests. Returns generated trades (empty if the
    // opposite side was empty). The book never takes ownership of a market order.
    virtual std::vector<Trade> add_market_order(Order* order) = 0;

    // Cancel a resting order by id. Returns true if an open/partial order was
    // found and removed; false if it does not exist or is already filled/cancelled.
    virtual bool cancel_order(uint64_t order_id) = 0;

    // Modify the remaining quantity of a resting order.
    //  - Reducing quantity (new_qty < current remaining) KEEPS time priority.
    //  - Increasing quantity LOSES time priority (order moves to the back of its level).
    // `new_qty` is the new *remaining* quantity target expressed as total size
    // relative to already-filled amount. Returns false if the order is unknown.
    virtual bool modify_order(uint64_t order_id, int64_t new_qty) = 0;

    // Non-mutating check: would submitting `order` as a limit order fill it
    // completely against current opposite-side liquidity (honoring its limit
    // price), leaving no resting remainder? Used to implement Fill-Or-Kill
    // (TIF_FOK) without ever partially mutating book state on failure. Returns
    // false for non-TYPE_LIMIT orders (FOK only applies to limit orders). Does
    // NOT account for self-trade prevention -- a same-trader resting order
    // still counts as available liquidity for this check; STP's cancellation
    // of that order happens only during the real, mutating match.
    virtual bool can_fill_completely(const Order* order) const = 0;

    // Best (highest) resting bid price, or NO_PRICE if no bids.
    virtual int64_t best_bid() const = 0;

    // Best (lowest) resting ask price, or NO_PRICE if no asks.
    virtual int64_t best_ask() const = 0;
};

}  // namespace me

#endif  // MATCHING_ENGINE_ORDER_BOOK_HPP
