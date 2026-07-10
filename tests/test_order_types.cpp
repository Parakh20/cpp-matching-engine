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

TEST_P(OrderBookTest, StopOrderRestsDormantThenTriggersAsMarket) {
    Order* liquidity_ask = mk(SIDE_ASK, TYPE_LIMIT, 5, 10100);
    engine_->submit_order(liquidity_ask);

    Order* buy_stop = mk(SIDE_BUY, TYPE_STOP, 5, /*price=*/0, /*trader_id=*/0,
                          TIF_GTC, /*stop_price=*/10050);
    auto dormant_trades = engine_->submit_order(buy_stop);
    EXPECT_TRUE(dormant_trades.empty());
    EXPECT_EQ(engine_->book().best_bid(), NO_PRICE);  // dormant, not in the live book

    Order* ask_trigger = mk(SIDE_ASK, TYPE_LIMIT, 1, 10060);
    engine_->submit_order(ask_trigger);
    Order* bid_trigger = mk(SIDE_BUY, TYPE_LIMIT, 1, 10060);
    auto trades = engine_->submit_order(bid_trigger);

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 10060);
    EXPECT_EQ(trades[1].price, 10100);
    EXPECT_EQ(trades[1].taker_order_id, buy_stop->order_id);
    EXPECT_EQ(buy_stop->type, TYPE_MARKET);
    EXPECT_EQ(buy_stop->status, STATUS_FILLED);
}

TEST_P(OrderBookTest, StopLimitOrderRestsDormantThenTriggersAsLimit) {
    Order* sell_stop_limit = mk(SIDE_ASK, TYPE_STOP_LIMIT, 5, /*price=*/9950,
                                 /*trader_id=*/0, TIF_GTC, /*stop_price=*/9980);
    auto dormant_trades = engine_->submit_order(sell_stop_limit);
    EXPECT_TRUE(dormant_trades.empty());
    EXPECT_EQ(engine_->book().best_ask(), NO_PRICE);

    // Trigger trade at 9970 (<= stop_price 9980) fires the sell stop-limit.
    Order* bid_trigger = mk(SIDE_BUY, TYPE_LIMIT, 1, 9970);
    engine_->submit_order(bid_trigger);
    Order* ask_trigger = mk(SIDE_ASK, TYPE_LIMIT, 1, 9970);
    auto trades = engine_->submit_order(ask_trigger);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 9970);
    EXPECT_EQ(sell_stop_limit->type, TYPE_LIMIT);
    EXPECT_EQ(engine_->book().best_ask(), 9950);  // converted limit rests at its own price
}

TEST_P(OrderBookTest, CascadingStopsTriggerEachOther) {
    // Resting liquidity the first triggered stop will consume, printing a
    // trade price that in turn triggers the second stop.
    Order* liquidity_ask = mk(SIDE_ASK, TYPE_LIMIT, 5, 10200);
    engine_->submit_order(liquidity_ask);

    Order* buy_stop_1 = mk(SIDE_BUY, TYPE_STOP, 5, /*price=*/0, /*trader_id=*/0,
                            TIF_GTC, /*stop_price=*/10050);
    Order* buy_stop_2 = mk(SIDE_BUY, TYPE_STOP, 5, /*price=*/0, /*trader_id=*/0,
                            TIF_GTC, /*stop_price=*/10150);
    engine_->submit_order(buy_stop_1);
    engine_->submit_order(buy_stop_2);

    // Trigger trade at 10060: fires buy_stop_1 (>=10050) but not buy_stop_2
    // (needs >=10150) until buy_stop_1's own fill (at 10200) cascades.
    Order* ask_trigger = mk(SIDE_ASK, TYPE_LIMIT, 1, 10060);
    engine_->submit_order(ask_trigger);
    Order* bid_trigger = mk(SIDE_BUY, TYPE_LIMIT, 1, 10060);
    auto trades = engine_->submit_order(bid_trigger);

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 10060);
    EXPECT_EQ(trades[1].price, 10200);
    EXPECT_EQ(trades[1].taker_order_id, buy_stop_1->order_id);
    EXPECT_EQ(buy_stop_1->status, STATUS_FILLED);
    // buy_stop_2 triggered (converted) but liquidity_ask was fully consumed
    // by buy_stop_1, so it finds no remaining liquidity and fills nothing.
    EXPECT_EQ(buy_stop_2->type, TYPE_MARKET);
    EXPECT_EQ(buy_stop_2->filled_qty, 0);
}

INSTANTIATE_TEST_SUITE_P(OrderTypeTests, OrderBookTest,
                         ::testing::Values(BookKind::Naive, BookKind::Optimized,
                                            BookKind::OptimizedArena));

}  // namespace me
