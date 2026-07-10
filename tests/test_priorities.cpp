#include "test_helpers.hpp"

#include "order.hpp"
#include "order_book.hpp"

namespace me {

TEST_P(OrderBookTest, PricePriorityBetterPriceFillsFirst) {
    // Best (lowest) ask should fill before a worse one, regardless of order
    // of insertion.
    Order* ask_worse = mk(SIDE_ASK, TYPE_LIMIT, 5, 10005);
    Order* ask_better = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    engine_->book().add_limit_order(ask_worse);
    engine_->book().add_limit_order(ask_better);

    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 5, 10010);
    auto trades = engine_->book().add_limit_order(bid);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, ask_better->order_id);
    EXPECT_EQ(trades[0].price, 10000);
}

TEST_P(OrderBookTest, TimePriorityFifoAtSamePrice) {
    Order* ask1 = mk(SIDE_ASK, TYPE_LIMIT, 3, 10000);
    Order* ask2 = mk(SIDE_ASK, TYPE_LIMIT, 3, 10000);
    Order* ask3 = mk(SIDE_ASK, TYPE_LIMIT, 3, 10000);
    engine_->book().add_limit_order(ask1);
    engine_->book().add_limit_order(ask2);
    engine_->book().add_limit_order(ask3);

    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 3, 10000);
    auto trades = engine_->book().add_limit_order(bid);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_order_id, ask1->order_id);  // strict FIFO
    EXPECT_EQ(ask1->status, STATUS_FILLED);
    EXPECT_EQ(ask2->status, STATUS_OPEN);
    EXPECT_EQ(ask3->status, STATUS_OPEN);
}

TEST_P(OrderBookTest, ThreeLevelSweepConsumesBestLevelsFirstInOrder) {
    Order* ask1 = mk(SIDE_ASK, TYPE_LIMIT, 4, 10000);
    Order* ask2 = mk(SIDE_ASK, TYPE_LIMIT, 4, 10001);
    Order* ask3 = mk(SIDE_ASK, TYPE_LIMIT, 4, 10002);
    engine_->book().add_limit_order(ask1);
    engine_->book().add_limit_order(ask2);
    engine_->book().add_limit_order(ask3);

    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 10, 10002);  // crosses all 3 levels
    auto trades = engine_->book().add_limit_order(bid);

    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].price, 10000);
    EXPECT_EQ(trades[0].quantity, 4);
    EXPECT_EQ(trades[1].price, 10001);
    EXPECT_EQ(trades[1].quantity, 4);
    EXPECT_EQ(trades[2].price, 10002);
    EXPECT_EQ(trades[2].quantity, 2);  // bid exhausted after 4+4+2=10
    EXPECT_EQ(engine_->book().best_ask(), 10002);  // ask3 has 2 left
    EXPECT_EQ(bid->status, STATUS_FILLED);
}

INSTANTIATE_TEST_SUITE_P(PriorityTests, OrderBookTest,
                         ::testing::Values(BookKind::Naive, BookKind::Optimized,
                                            BookKind::OptimizedArena));

}  // namespace me
