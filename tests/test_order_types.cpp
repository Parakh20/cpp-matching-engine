#include "test_helpers.hpp"

#include "order.hpp"
#include "order_book.hpp"

namespace me {

TEST_P(OrderBookTest, IOCPartialFillDiscardsRemainder) {
    Order* ask = mk(SIDE_ASK, TYPE_LIMIT, 3, 10000);
    engine_->book().add_limit_order(ask);

    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 5, 10000, /*trader_id=*/0, TIF_IOC);
    auto trades = engine_->book().add_limit_order(bid);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 3);
    EXPECT_EQ(bid->filled_qty, 3);
    EXPECT_EQ(bid->status, STATUS_PARTIAL);
    EXPECT_EQ(engine_->book().best_bid(), NO_PRICE);  // remainder discarded, not resting
}

TEST_P(OrderBookTest, FOKSucceedsWhenLiquiditySufficient) {
    Order* ask1 = mk(SIDE_ASK, TYPE_LIMIT, 3, 10000);
    Order* ask2 = mk(SIDE_ASK, TYPE_LIMIT, 3, 10000);
    engine_->book().add_limit_order(ask1);
    engine_->book().add_limit_order(ask2);

    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 5, 10000, /*trader_id=*/0, TIF_FOK);
    auto trades = engine_->book().add_limit_order(bid);

    int64_t filled = 0;
    for (const auto& t : trades) filled += t.quantity;
    EXPECT_EQ(filled, 5);
    EXPECT_EQ(bid->status, STATUS_FILLED);
}

TEST_P(OrderBookTest, FOKFailsSafelyWhenLiquidityInsufficient) {
    Order* ask = mk(SIDE_ASK, TYPE_LIMIT, 3, 10000);
    engine_->book().add_limit_order(ask);
    ASSERT_EQ(engine_->book().best_ask(), 10000);

    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 5, 10000, /*trader_id=*/0, TIF_FOK);
    auto trades = engine_->book().add_limit_order(bid);

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(bid->filled_qty, 0);
    EXPECT_EQ(engine_->book().best_ask(), 10000);   // untouched
    EXPECT_EQ(engine_->book().best_bid(), NO_PRICE);  // FOK order never rests either
}

TEST_P(OrderBookTest, STPCancelsRestingOrderSameTrader) {
    Order* ask = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000, /*trader_id=*/42);
    engine_->book().add_limit_order(ask);

    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 5, 10000, /*trader_id=*/42);
    auto trades = engine_->book().add_limit_order(bid);

    EXPECT_TRUE(trades.empty());                // no self-trade
    EXPECT_EQ(ask->status, STATUS_CANCELLED);    // resting order cancelled, not filled
    EXPECT_EQ(engine_->book().best_ask(), NO_PRICE);
    EXPECT_EQ(engine_->book().best_bid(), 10000);  // incoming bid rests (GTC default), no counterparty left
}

TEST_P(OrderBookTest, STPSkipsSelfThenMatchesNextEligibleOrder) {
    Order* ask_self = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000, /*trader_id=*/42);
    Order* ask_other = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000, /*trader_id=*/7);
    engine_->book().add_limit_order(ask_self);
    engine_->book().add_limit_order(ask_other);

    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 5, 10000, /*trader_id=*/42);
    auto trades = engine_->book().add_limit_order(bid);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, ask_other->order_id);
    EXPECT_EQ(ask_self->status, STATUS_CANCELLED);
    EXPECT_EQ(bid->status, STATUS_FILLED);
}

INSTANTIATE_TEST_SUITE_P(OrderTypeTests, OrderBookTest,
                         ::testing::Values(BookKind::Naive, BookKind::Optimized,
                                            BookKind::OptimizedArena));

}  // namespace me
