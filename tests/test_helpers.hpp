// Shared fixture for the gtest suite. Parameterized over BookKind so every
// TEST_P body runs unchanged against V1 (Naive), V2 (Optimized), and V3
// (OptimizedArena) -- the single strongest correctness proof that the three
// engines are behaviorally equivalent.
#ifndef MATCHING_ENGINE_TEST_HELPERS_HPP
#define MATCHING_ENGINE_TEST_HELPERS_HPP

#include <gtest/gtest.h>

#include <vector>

#include "engine.hpp"
#include "order.hpp"

namespace me {

class OrderBookTest : public ::testing::TestWithParam<BookKind> {
protected:
    void SetUp() override {
        // A generous window around a $100.00 mid (price units = cents-ticks).
        engine_ = std::make_unique<Engine>(GetParam(), /*min_price=*/0, /*tick=*/1);
    }

    void TearDown() override {
        for (Order* o : owned_orders_) delete o;
        owned_orders_.clear();
    }

    // Test-owned Order allocation (freed in TearDown). The book never
    // allocates/frees Order memory itself -- see src/optimized/order_book.cpp.
    Order* mk(char side, char type, int64_t qty, int64_t price = 0,
              uint64_t trader_id = 0, char tif = TIF_GTC, int64_t stop_price = 0) {
        Order* o = new Order();
        o->order_id = engine_->next_order_id();
        o->timestamp_ns = engine_->next_timestamp();
        o->side = side;
        o->type = type;
        o->quantity = qty;
        o->price = price;
        o->trader_id = trader_id;
        o->tif = tif;
        o->stop_price = stop_price;
        owned_orders_.push_back(o);
        return o;
    }

    std::unique_ptr<Engine> engine_;
    std::vector<Order*> owned_orders_;
};

}  // namespace me

#endif  // MATCHING_ENGINE_TEST_HELPERS_HPP
