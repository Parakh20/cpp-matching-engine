#include "test_helpers.hpp"

#include "order.hpp"
#include "order_book.hpp"

namespace me {

TEST_P(OrderBookTest, CancelNonExistentOrderReturnsFalse) {
    EXPECT_FALSE(engine_->book().cancel_order(999999));
}

TEST_P(OrderBookTest, CancelAlreadyFilledOrderReturnsFalse) {
    Order* ask = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    engine_->book().add_limit_order(ask);
    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 5, 10000);
    engine_->book().add_limit_order(bid);  // fully fills ask

    EXPECT_FALSE(engine_->book().cancel_order(ask->order_id));
}

TEST_P(OrderBookTest, MarketOrderOnEmptyBookYieldsZeroFills) {
    Order* bid = mk(SIDE_BUY, TYPE_MARKET, 5);
    auto trades = engine_->book().add_market_order(bid);
    EXPECT_TRUE(trades.empty());
}

TEST_P(OrderBookTest, CrossedLimitOrderMatchesImmediately) {
    Order* ask = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    engine_->book().add_limit_order(ask);

    // Bid priced ABOVE the best ask -- must match immediately, not rest.
    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 5, 10050);
    auto trades = engine_->book().add_limit_order(bid);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 10000);  // still prints at maker price, not bid's limit
    EXPECT_EQ(bid->status, STATUS_FILLED);
}

TEST_P(OrderBookTest, ModifyReduceKeepsTimePriority) {
    Order* ask1 = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    Order* ask2 = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    engine_->book().add_limit_order(ask1);
    engine_->book().add_limit_order(ask2);

    ASSERT_TRUE(engine_->book().modify_order(ask1->order_id, 2));  // reduce 5 -> 2
    EXPECT_EQ(ask1->quantity, 2);

    // ask1 must still fill FIRST (kept its place at the front of the queue).
    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 2, 10000);
    auto trades = engine_->book().add_limit_order(bid);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, ask1->order_id);
}

TEST_P(OrderBookTest, ModifyIncreaseLosesTimePriority) {
    Order* ask1 = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    Order* ask2 = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    engine_->book().add_limit_order(ask1);
    engine_->book().add_limit_order(ask2);

    ASSERT_TRUE(engine_->book().modify_order(ask1->order_id, 10));  // increase 5 -> 10

    // ask1 lost its place; ask2 (still original size) must fill FIRST now.
    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 5, 10000);
    auto trades = engine_->book().add_limit_order(bid);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, ask2->order_id);
}

TEST_P(OrderBookTest, ModifyToZeroOrBelowFilledActsAsCancel) {
    Order* ask = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    engine_->book().add_limit_order(ask);

    ASSERT_TRUE(engine_->book().modify_order(ask->order_id, 0));
    EXPECT_EQ(ask->status, STATUS_FILLED);  // new_qty <= filled_qty (0) path
    EXPECT_EQ(engine_->book().best_ask(), NO_PRICE);
}

TEST_P(OrderBookTest, ModifyNonExistentOrderReturnsFalse) {
    EXPECT_FALSE(engine_->book().modify_order(999999, 5));
}

TEST_P(OrderBookTest, CancelRemovesFromBestPriceAndRescansCorrectly) {
    Order* ask_best = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    Order* ask_next = mk(SIDE_ASK, TYPE_LIMIT, 5, 10001);
    engine_->book().add_limit_order(ask_best);
    engine_->book().add_limit_order(ask_next);
    ASSERT_EQ(engine_->book().best_ask(), 10000);

    ASSERT_TRUE(engine_->book().cancel_order(ask_best->order_id));
    EXPECT_EQ(engine_->book().best_ask(), 10001);  // must rescan to the next level
}

INSTANTIATE_TEST_SUITE_P(EdgeCaseTests, OrderBookTest,
                         ::testing::Values(BookKind::Naive, BookKind::Optimized,
                                            BookKind::OptimizedArena));

}  // namespace me
