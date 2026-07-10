#include "engine.hpp"

namespace me {

// Defined in src/naive/order_book.cpp.
IOrderBook* create_naive_order_book();

// Defined in src/optimized/order_book.cpp. V2 and V3 share the exact same
// data structures and matching logic (array-indexed price levels + intrusive
// list) -- the only difference between them is which allocator the *caller*
// uses to create Order objects before handing them to the book (heap new/delete
// for V2, Arena for V3). The book itself never allocates or frees Order memory
// (matching V1's behavior: ownership transfer described in order_book.hpp is
// about the book's internal FIFO containers, not literal free() responsibility),
// so one class correctly serves both BookKind values.
IOrderBook* create_optimized_order_book(int64_t min_price, int64_t tick);

std::unique_ptr<IOrderBook> make_order_book(BookKind kind, int64_t min_price, int64_t tick) {
    switch (kind) {
        case BookKind::Naive:
            return std::unique_ptr<IOrderBook>(create_naive_order_book());
        case BookKind::Optimized:
        case BookKind::OptimizedArena:
            return std::unique_ptr<IOrderBook>(create_optimized_order_book(min_price, tick));
    }
    return nullptr;  // unreachable; silences -Wreturn-type on some compilers
}

std::vector<Trade> Engine::dispatch_to_book(Order* order) {
    return (order->type == TYPE_MARKET) ? book_->add_market_order(order)
                                         : book_->add_limit_order(order);
}

Order* Engine::pop_triggerable_buy_stop(int64_t last_trade_price) {
    for (auto it = dormant_buy_stops_.begin(); it != dormant_buy_stops_.end(); ++it) {
        if (last_trade_price >= (*it)->stop_price) {
            Order* stop = *it;
            dormant_buy_stops_.erase(it);
            return stop;
        }
    }
    return nullptr;
}

Order* Engine::pop_triggerable_sell_stop(int64_t last_trade_price) {
    for (auto it = dormant_sell_stops_.begin(); it != dormant_sell_stops_.end(); ++it) {
        if (last_trade_price <= (*it)->stop_price) {
            Order* stop = *it;
            dormant_sell_stops_.erase(it);
            return stop;
        }
    }
    return nullptr;
}

void Engine::trigger_ready_stops(int64_t last_trade_price, std::vector<Trade>& out_trades) {
    bool triggered_any = true;
    while (triggered_any) {
        triggered_any = false;

        Order* stop = pop_triggerable_buy_stop(last_trade_price);
        if (stop == nullptr) stop = pop_triggerable_sell_stop(last_trade_price);
        if (stop == nullptr) break;

        stop->type = (stop->type == TYPE_STOP) ? TYPE_MARKET : TYPE_LIMIT;
        std::vector<Trade> cascade = dispatch_to_book(stop);
        out_trades.insert(out_trades.end(), cascade.begin(), cascade.end());
        if (!cascade.empty()) last_trade_price = cascade.back().price;
        triggered_any = true;
    }
}

std::vector<Trade> Engine::submit_order(Order* order) {
    if (order->type == TYPE_STOP || order->type == TYPE_STOP_LIMIT) {
        order->status = STATUS_OPEN;
        (order->side == SIDE_BUY ? dormant_buy_stops_ : dormant_sell_stops_).push_back(order);
        return {};  // dormant: never touches the live book until triggered
    }

    std::vector<Trade> all_trades = dispatch_to_book(order);
    if (!all_trades.empty()) {
        trigger_ready_stops(all_trades.back().price, all_trades);
    }
    return all_trades;
}

}  // namespace me
