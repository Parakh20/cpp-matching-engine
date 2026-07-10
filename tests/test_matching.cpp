#include "test_helpers.hpp"

#include "order.hpp"
#include "order_book.hpp"

namespace me {

TEST_P(OrderBookTest, SingleBidCrossesSingleAskAtMakerPrice) {
    Order* ask = mk(SIDE_ASK, TYPE_LIMIT, 10, 10000);  // rests at $100.00
    auto rest_trades = engine_->book().add_limit_order(ask);
    EXPECT_TRUE(rest_trades.empty());

    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 10, 10005);  // crosses at $100.05
    auto trades = engine_->book().add_limit_order(bid);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 10000);  // prints at the MAKER's (ask's) price
    EXPECT_EQ(trades[0].quantity, 10);
    EXPECT_EQ(trades[0].maker_order_id, ask->order_id);
    EXPECT_EQ(trades[0].taker_order_id, bid->order_id);
    EXPECT_EQ(ask->status, STATUS_FILLED);
    EXPECT_EQ(bid->status, STATUS_FILLED);
    EXPECT_EQ(engine_->book().best_ask(), NO_PRICE);
    EXPECT_EQ(engine_->book().best_bid(), NO_PRICE);
}

TEST_P(OrderBookTest, PartialFillOneBigOrderEatsSeveralSmallOnes) {
    Order* ask1 = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    Order* ask2 = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    Order* ask3 = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    engine_->book().add_limit_order(ask1);
    engine_->book().add_limit_order(ask2);
    engine_->book().add_limit_order(ask3);

    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 12, 10000);
    auto trades = engine_->book().add_limit_order(bid);

    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].maker_order_id, ask1->order_id);  // FIFO: ask1 first
    EXPECT_EQ(trades[0].quantity, 5);
    EXPECT_EQ(trades[1].maker_order_id, ask2->order_id);
    EXPECT_EQ(trades[1].quantity, 5);
    EXPECT_EQ(trades[2].maker_order_id, ask3->order_id);
    EXPECT_EQ(trades[2].quantity, 2);  // only 2 left to fill the bid
    EXPECT_EQ(ask3->status, STATUS_PARTIAL);
    EXPECT_EQ(ask3->remaining(), 3);
    EXPECT_EQ(bid->status, STATUS_FILLED);
}

TEST_P(OrderBookTest, MarketOrderWalksMultipleLevels) {
    Order* ask1 = mk(SIDE_ASK, TYPE_LIMIT, 5, 10000);
    Order* ask2 = mk(SIDE_ASK, TYPE_LIMIT, 5, 10001);
    engine_->book().add_limit_order(ask1);
    engine_->book().add_limit_order(ask2);

    Order* bid = mk(SIDE_BUY, TYPE_MARKET, 8);
    auto trades = engine_->book().add_market_order(bid);

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 10000);  // best level first
    EXPECT_EQ(trades[0].quantity, 5);
    EXPECT_EQ(trades[1].price, 10001);
    EXPECT_EQ(trades[1].quantity, 3);
    EXPECT_EQ(bid->status, STATUS_FILLED);
    EXPECT_EQ(engine_->book().best_ask(), 10001);  // ask2 partially remains
}

TEST_P(OrderBookTest, NonCrossingLimitOrderRestsOnBook) {
    Order* bid = mk(SIDE_BUY, TYPE_LIMIT, 10, 9900);
    auto trades = engine_->book().add_limit_order(bid);

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(bid->status, STATUS_OPEN);
    EXPECT_EQ(engine_->book().best_bid(), 9900);
    EXPECT_EQ(engine_->book().best_ask(), NO_PRICE);
}

TEST_P(OrderBookTest, MarketOrderOnEmptyBookProducesNoFills) {
    Order* bid = mk(SIDE_BUY, TYPE_MARKET, 10);
    auto trades = engine_->book().add_market_order(bid);

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(bid->status, STATUS_OPEN);
}

INSTANTIATE_TEST_SUITE_P(MatchingTests, OrderBookTest,
                         ::testing::Values(BookKind::Naive, BookKind::Optimized,
                                            BookKind::OptimizedArena));

}  // namespace me
