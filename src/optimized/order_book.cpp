// V2/V3 - Optimized order book: array-indexed price levels (O(1) best-price
// lookup, replacing V1's O(log n) std::map) plus an intrusive doubly-linked
// list per level threaded through Order::prev/next (O(1) FIFO append and,
// critically, O(1) cancel-from-anywhere -- no epoch/tombstone scheme needed,
// because unlike std::queue an intrusive list supports true middle-removal).
//
// V2 and V3 are the SAME class. The only difference between them is which
// allocator the *caller* uses to create Order objects before handing them to
// add_limit_order/add_market_order (heap new/delete for V2, Arena bump-pointer
// allocation for V3, see benchmark/bench_insert.cpp) -- this book never
// allocates or frees Order memory itself, matching V1's actual behavior.
//
// Price levels are indexed as (price - min_price) / tick into a fixed-size
// array. best_bid_idx_/best_ask_idx_ cache the current touch and are updated
// in O(1) on every insert that improves the best price; removing the last
// order at the current best level triggers a bounded linear rescan for the
// next-best occupied level (the standard array-book tradeoff: O(1) in the
// common case, O(price range) in the worst case, still far cheaper in
// practice than std::map's pointer-chasing rebalance).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "order.hpp"
#include "order_book.hpp"
#include "trade.hpp"

namespace me {

namespace {

uint64_t g_next_trade_id = 1000000;   // disjoint id space from the naive book
uint64_t g_synthetic_clock = 1000000;

constexpr size_t DEFAULT_PRICE_LEVELS = 20000;

}  // namespace

class OptimizedOrderBook final : public IOrderBook {
public:
    OptimizedOrderBook(int64_t min_price, int64_t tick, size_t num_levels = DEFAULT_PRICE_LEVELS)
        : min_price_(min_price),
          tick_(tick <= 0 ? 1 : tick),
          bid_levels_(num_levels),
          ask_levels_(num_levels) {}

    std::vector<Trade> add_limit_order(Order* order) override;
    std::vector<Trade> add_market_order(Order* order) override;
    bool cancel_order(uint64_t order_id) override;
    bool modify_order(uint64_t order_id, int64_t new_qty) override;
    int64_t best_bid() const override;
    int64_t best_ask() const override;

private:
    struct Level {
        int64_t total_qty = 0;
        Order* head = nullptr;  // oldest (first in time priority)
        Order* tail = nullptr;  // newest
        bool empty() const { return head == nullptr; }
    };

    int64_t min_price_;
    int64_t tick_;
    std::vector<Level> bid_levels_;
    std::vector<Level> ask_levels_;
    std::unordered_map<uint64_t, Order*> order_index_;
    int64_t best_bid_idx_ = -1;  // -1 == no resting bids
    int64_t best_ask_idx_ = -1;  // -1 == no resting asks

    int64_t index_of(int64_t price) const { return (price - min_price_) / tick_; }
    bool in_range(int64_t idx, const std::vector<Level>& levels) const {
        return idx >= 0 && static_cast<size_t>(idx) < levels.size();
    }

    // Pure intrusive-list plumbing; total_qty bookkeeping is the caller's job
    // (different call sites adjust it by different deltas: full remove vs.
    // partial fill vs. quantity-reduce-in-place).
    static void append_node(Level& level, Order* order) {
        order->prev = level.tail;
        order->next = nullptr;
        if (level.tail != nullptr) level.tail->next = order;
        level.tail = order;
        if (level.head == nullptr) level.head = order;
    }

    static void remove_node(Level& level, Order* order) {
        if (order->prev != nullptr) order->prev->next = order->next; else level.head = order->next;
        if (order->next != nullptr) order->next->prev = order->prev; else level.tail = order->prev;
        order->prev = nullptr;
        order->next = nullptr;
    }

    void rescan_best_bid() {
        for (int64_t i = best_bid_idx_; i >= 0; --i) {
            if (!bid_levels_[static_cast<size_t>(i)].empty()) {
                best_bid_idx_ = i;
                return;
            }
        }
        best_bid_idx_ = -1;
    }

    void rescan_best_ask() {
        for (size_t i = static_cast<size_t>(best_ask_idx_); i < ask_levels_.size(); ++i) {
            if (!ask_levels_[i].empty()) {
                best_ask_idx_ = static_cast<int64_t>(i);
                return;
            }
        }
        best_ask_idx_ = -1;
    }

    Trade make_trade(const Order* maker, const Order* taker, int64_t qty, char aggressor) {
        Trade t;
        t.trade_id = g_next_trade_id++;
        t.timestamp_ns = g_synthetic_clock++;
        t.price = maker->price;  // trades print at the maker (resting) price
        t.quantity = qty;
        t.maker_order_id = maker->order_id;
        t.taker_order_id = taker->order_id;
        t.aggressor_side = aggressor;
        return t;
    }

    void rest(Order* order);
    std::vector<Trade> match(Order* order, bool is_market);
};

void OptimizedOrderBook::rest(Order* order) {
    const bool buying = (order->side == SIDE_BUY);
    std::vector<Level>& levels = buying ? bid_levels_ : ask_levels_;
    const int64_t idx = index_of(order->price);
    if (!in_range(idx, levels)) return;  // price outside the configured window; drop silently

    Level& level = levels[static_cast<size_t>(idx)];
    append_node(level, order);
    level.total_qty += order->remaining();
    order_index_[order->order_id] = order;

    int64_t& best = buying ? best_bid_idx_ : best_ask_idx_;
    if (buying) {
        if (best == -1 || idx > best) best = idx;
    } else {
        if (best == -1 || idx < best) best = idx;
    }
}

std::vector<Trade> OptimizedOrderBook::match(Order* order, bool is_market) {
    std::vector<Trade> trades;
    const bool buying = (order->side == SIDE_BUY);
    std::vector<Level>& opp_levels = buying ? ask_levels_ : bid_levels_;
    int64_t& opp_best = buying ? best_ask_idx_ : best_bid_idx_;

    while (order->remaining() > 0 && opp_best != -1) {
        Level& level = opp_levels[static_cast<size_t>(opp_best)];
        if (level.empty()) {
            if (buying) rescan_best_ask(); else rescan_best_bid();
            continue;
        }

        const int64_t level_price = min_price_ + opp_best * tick_;
        if (!is_market) {
            const bool crosses = buying ? (level_price <= order->price)
                                         : (level_price >= order->price);
            if (!crosses) break;
        }

        while (order->remaining() > 0 && !level.empty()) {
            Order* maker = level.head;
            const int64_t fill = std::min(order->remaining(), maker->remaining());
            maker->filled_qty += fill;
            order->filled_qty += fill;
            level.total_qty -= fill;
            maker->status = (maker->remaining() == 0) ? STATUS_FILLED : STATUS_PARTIAL;

            trades.push_back(make_trade(maker, order, fill, buying ? SIDE_BUY : SIDE_ASK));

            if (maker->remaining() == 0) {
                remove_node(level, maker);
                order_index_.erase(maker->order_id);
            }
        }
        if (level.empty()) {
            if (buying) rescan_best_ask(); else rescan_best_bid();
        }
    }
    return trades;
}

std::vector<Trade> OptimizedOrderBook::add_limit_order(Order* order) {
    order->status = STATUS_OPEN;
    std::vector<Trade> trades = match(order, /*is_market=*/false);

    if (order->remaining() > 0) {
        order->status = (order->filled_qty > 0) ? STATUS_PARTIAL : STATUS_OPEN;
        rest(order);
    } else {
        order->status = STATUS_FILLED;
    }
    return trades;
}

std::vector<Trade> OptimizedOrderBook::add_market_order(Order* order) {
    order->status = STATUS_OPEN;
    std::vector<Trade> trades = match(order, /*is_market=*/true);
    order->status = (order->remaining() == 0)
                        ? STATUS_FILLED
                        : (order->filled_qty > 0 ? STATUS_PARTIAL : STATUS_OPEN);
    return trades;  // market orders never rest
}

bool OptimizedOrderBook::cancel_order(uint64_t order_id) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;
    Order* o = it->second;
    if (o == nullptr || o->status == STATUS_CANCELLED || o->status == STATUS_FILLED ||
        o->remaining() <= 0) {
        return false;
    }

    const bool buying = (o->side == SIDE_BUY);
    std::vector<Level>& levels = buying ? bid_levels_ : ask_levels_;
    const int64_t idx = index_of(o->price);
    Level& level = levels[static_cast<size_t>(idx)];

    level.total_qty -= o->remaining();
    remove_node(level, o);
    o->status = STATUS_CANCELLED;
    order_index_.erase(it);

    int64_t& best = buying ? best_bid_idx_ : best_ask_idx_;
    if (level.empty() && best == idx) {
        if (buying) rescan_best_bid(); else rescan_best_ask();
    }
    return true;
}

bool OptimizedOrderBook::modify_order(uint64_t order_id, int64_t new_qty) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;
    Order* o = it->second;
    if (o == nullptr || o->status == STATUS_CANCELLED || o->status == STATUS_FILLED ||
        o->remaining() <= 0) {
        return false;
    }

    const bool buying = (o->side == SIDE_BUY);
    std::vector<Level>& levels = buying ? bid_levels_ : ask_levels_;
    const int64_t idx = index_of(o->price);
    Level& level = levels[static_cast<size_t>(idx)];
    int64_t& best = buying ? best_bid_idx_ : best_ask_idx_;

    // new_qty is the new TOTAL order quantity.
    if (new_qty <= o->filled_qty) {
        level.total_qty -= o->remaining();
        remove_node(level, o);
        o->quantity = o->filled_qty;
        o->status = STATUS_FILLED;
        order_index_.erase(it);
        if (level.empty() && best == idx) {
            if (buying) rescan_best_bid(); else rescan_best_ask();
        }
        return true;
    }

    const int64_t old_remaining = o->remaining();
    const int64_t new_remaining = new_qty - o->filled_qty;

    if (new_remaining <= old_remaining) {
        // Reduce -> keep time priority: mutate in place, node stays where it is.
        level.total_qty -= (old_remaining - new_remaining);
        o->quantity = new_qty;
        o->status = (o->filled_qty > 0) ? STATUS_PARTIAL : STATUS_OPEN;
        return true;
    }

    // Increase -> lose time priority: unlink, then re-append at the tail.
    level.total_qty -= old_remaining;
    remove_node(level, o);
    o->quantity = new_qty;
    o->status = (o->filled_qty > 0) ? STATUS_PARTIAL : STATUS_OPEN;
    append_node(level, o);
    level.total_qty += o->remaining();
    return true;
}

int64_t OptimizedOrderBook::best_bid() const {
    return best_bid_idx_ == -1 ? NO_PRICE : min_price_ + best_bid_idx_ * tick_;
}

int64_t OptimizedOrderBook::best_ask() const {
    return best_ask_idx_ == -1 ? NO_PRICE : min_price_ + best_ask_idx_ * tick_;
}

// Factory hook used by engine.cpp. Serves both BookKind::Optimized (V2) and
// BookKind::OptimizedArena (V3) -- see the comment in engine.cpp.
IOrderBook* create_optimized_order_book(int64_t min_price, int64_t tick) {
    return new OptimizedOrderBook(min_price, tick);
}

}  // namespace me
