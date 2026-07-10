#ifndef MATCHING_ENGINE_TRADE_HPP
#define MATCHING_ENGINE_TRADE_HPP

#include <cstdint>

namespace me {

// A single execution between a resting (maker) order and an incoming (taker)
// order. Trades always print at the MAKER's price (price-time priority), which
// is the invariant the gtest suite pins down for every engine version.
struct Trade {
    uint64_t trade_id = 0;
    uint64_t timestamp_ns = 0;
    int64_t price = 0;      // maker price, integer ticks
    int64_t quantity = 0;   // matched quantity
    uint64_t maker_order_id = 0;
    uint64_t taker_order_id = 0;
    char aggressor_side = 'B';  // side of the taker ('B' or 'A')
};

}  // namespace me

#endif  // MATCHING_ENGINE_TRADE_HPP
