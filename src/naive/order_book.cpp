// V1 - Naive order book: std::map<price, std::queue<Slot>> per side plus an
// unordered_map index for cancel/modify. Optimized for clarity, not speed:
//   - price lookup is O(log n) through the tree map;
//   - each resting order lives in a std::queue node (per-node heap allocation);
//   - cancellation is lazy (tombstone + skip), because you cannot erase from the
//     middle of a std::queue.
// This is the correctness baseline; the gtest suite is written against its
// observable behavior and V2/V3 must reproduce it exactly.
//
// A queue holds (Order*, epoch) Slots. Each order has a "live epoch" tracked in
// `epoch_`. A queue slot is live only if its epoch matches the order's current
// live epoch. Cancelling bumps the epoch (all old slots die). modify-increase
// bumps the epoch and pushes a new slot at the back (loses time priority) while
// any stale slot is skipped. This cleanly handles the same Order pointer
// appearing in more than one slot.

#include <algorithm>
#include <cstdint>
#include <map>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

#include "order.hpp"
#include "order_book.hpp"
#include "trade.hpp"

namespace me {

namespace {

uint64_t g_next_trade_id = 1;
uint64_t g_synthetic_clock = 1;

}  // namespace

class NaiveOrderBook final : public IOrderBook {
public:
    std::vector<Trade> add_limit_order(Order* order) override;
    std::vector<Trade> add_market_order(Order* order) override;
    bool cancel_order(uint64_t order_id) override;
    bool modify_order(uint64_t order_id, int64_t new_qty) override;
    int64_t best_bid() const override;
    int64_t best_ask() const override;

private:
    struct Slot {
        Order* order;
        uint64_t epoch;  // slot is valid iff epoch == live epoch of order
    };

    // bids: HIGHEST price first -> iterate with rbegin().
    // asks: LOWEST price first  -> iterate with begin().
    std::map<int64_t, std::queue<Slot>> bids_;
    std::map<int64_t, std::queue<Slot>> asks_;
    std::unordered_map<uint64_t, Order*> order_index_;
    std::unordered_map<uint64_t, uint64_t> epoch_;  // order_id -> current live epoch

    uint64_t live_epoch(uint64_t order_id) const {
        auto it = epoch_.find(order_id);
        return it == epoch_.end() ? 0 : it->second;
    }

    bool slot_live(const Slot& s) const {
        const Order* o = s.order;
        return o != nullptr && o->status != STATUS_CANCELLED &&
               o->remaining() > 0 && s.epoch == live_epoch(o->order_id);
    }

    void drop_front(std::map<int64_t, std::queue<Slot>>& side, bool ascending);

    Trade make_trade(const Order* maker, const Order* taker, int64_t qty,
                     char aggressor) {
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

void NaiveOrderBook::drop_front(std::map<int64_t, std::queue<Slot>>& side,
                                bool ascending) {
    while (!side.empty()) {
        auto it = ascending ? side.begin() : std::prev(side.end());
        std::queue<Slot>& q = it->second;
        while (!q.empty() && !slot_live(q.front())) {
            q.pop();
        }
        if (q.empty()) {
            side.erase(it);
        } else {
            break;
        }
    }
}

int64_t NaiveOrderBook::best_bid() const {
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
        std::queue<Slot> copy = it->second;
        while (!copy.empty()) {
            if (slot_live(copy.front())) return it->first;
            copy.pop();
        }
    }
    return NO_PRICE;
}

int64_t NaiveOrderBook::best_ask() const {
    for (auto it = asks_.begin(); it != asks_.end(); ++it) {
        std::queue<Slot> copy = it->second;
        while (!copy.empty()) {
            if (slot_live(copy.front())) return it->first;
            copy.pop();
        }
    }
    return NO_PRICE;
}

void NaiveOrderBook::rest(Order* order) {
    const bool buying = (order->side == SIDE_BUY);
    std::map<int64_t, std::queue<Slot>>& own = buying ? bids_ : asks_;
    const uint64_t ep = ++epoch_[order->order_id];  // fresh live epoch
    own[order->price].push(Slot{order, ep});
    order_index_[order->order_id] = order;
}

std::vector<Trade> NaiveOrderBook::match(Order* order, bool is_market) {
    std::vector<Trade> trades;
    const bool buying = (order->side == SIDE_BUY);
    std::map<int64_t, std::queue<Slot>>& opp = buying ? asks_ : bids_;

    while (order->remaining() > 0 && !opp.empty()) {
        drop_front(opp, /*ascending=*/buying);  // asks ascending, bids descending
        if (opp.empty()) break;

        auto best_it = buying ? opp.begin() : std::prev(opp.end());
        const int64_t maker_price = best_it->first;

        if (!is_market) {
            const bool crosses = buying ? (maker_price <= order->price)
                                        : (maker_price >= order->price);
            if (!crosses) break;
        }

        std::queue<Slot>& q = best_it->second;
        while (order->remaining() > 0 && !q.empty()) {
            Slot front = q.front();
            if (!slot_live(front)) {
                q.pop();
                continue;
            }
            Order* maker = front.order;
            const int64_t fill = std::min(order->remaining(), maker->remaining());
            maker->filled_qty += fill;
            order->filled_qty += fill;
            maker->status =
                (maker->remaining() == 0) ? STATUS_FILLED : STATUS_PARTIAL;

            trades.push_back(
                make_trade(maker, order, fill, buying ? SIDE_BUY : SIDE_ASK));

            if (maker->remaining() == 0) {
                order_index_.erase(maker->order_id);
                epoch_.erase(maker->order_id);
                q.pop();
            }
        }
        if (q.empty()) {
            opp.erase(best_it);
        }
    }
    return trades;
}

std::vector<Trade> NaiveOrderBook::add_limit_order(Order* order) {
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

std::vector<Trade> NaiveOrderBook::add_market_order(Order* order) {
    order->status = STATUS_OPEN;
    std::vector<Trade> trades = match(order, /*is_market=*/true);
    order->status = (order->remaining() == 0)
                        ? STATUS_FILLED
                        : (order->filled_qty > 0 ? STATUS_PARTIAL : STATUS_OPEN);
    return trades;  // market orders never rest
}

bool NaiveOrderBook::cancel_order(uint64_t order_id) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;
    Order* o = it->second;
    if (o == nullptr || o->status == STATUS_CANCELLED ||
        o->status == STATUS_FILLED || o->remaining() <= 0) {
        return false;
    }
    o->status = STATUS_CANCELLED;  // tombstone; bump epoch so slots die
    ++epoch_[order_id];
    order_index_.erase(it);
    return true;
}

bool NaiveOrderBook::modify_order(uint64_t order_id, int64_t new_qty) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;
    Order* o = it->second;
    if (o == nullptr || o->status == STATUS_CANCELLED ||
        o->status == STATUS_FILLED || o->remaining() <= 0) {
        return false;
    }

    // new_qty is the new TOTAL order quantity.
    if (new_qty <= o->filled_qty) {
        o->quantity = o->filled_qty;
        o->status = STATUS_FILLED;
        ++epoch_[order_id];
        order_index_.erase(it);
        epoch_.erase(order_id);
        return true;
    }

    const int64_t old_remaining = o->remaining();
    const int64_t new_remaining = new_qty - o->filled_qty;

    if (new_remaining <= old_remaining) {
        // Reduce -> keep time priority: mutate in place, same slot stays valid.
        o->quantity = new_qty;
        o->status = (o->filled_qty > 0) ? STATUS_PARTIAL : STATUS_OPEN;
        return true;
    }

    // Increase -> lose time priority: bump epoch (old slot dies) and push a new
    // slot at the back of the same price level.
    o->quantity = new_qty;
    o->status = (o->filled_qty > 0) ? STATUS_PARTIAL : STATUS_OPEN;
    const uint64_t ep = ++epoch_[order_id];
    const bool buying = (o->side == SIDE_BUY);
    std::map<int64_t, std::queue<Slot>>& own = buying ? bids_ : asks_;
    own[o->price].push(Slot{o, ep});
    order_index_[order_id] = o;
    return true;
}

// Factory hook used by engine.cpp.
IOrderBook* create_naive_order_book() { return new NaiveOrderBook(); }

}  // namespace me
