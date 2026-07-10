#ifndef MATCHING_ENGINE_ORDER_HPP
#define MATCHING_ENGINE_ORDER_HPP

#include <cstdint>

namespace me {

// Order side. Single char keeps the struct compact and branch-friendly.
constexpr char SIDE_BUY = 'B';
constexpr char SIDE_ASK = 'A';

// Order type.
constexpr char TYPE_LIMIT = 'L';
constexpr char TYPE_MARKET = 'M';
constexpr char TYPE_STOP = 'S';        // dormant until stop_price triggers; converts to TYPE_MARKET
constexpr char TYPE_STOP_LIMIT = 'P';  // dormant until stop_price triggers; converts to TYPE_LIMIT

// Time-in-force, applies to TYPE_LIMIT orders (and the post-trigger Limit that
// a TYPE_STOP_LIMIT order converts into).
constexpr char TIF_GTC = 'G';  // Good-Til-Cancel: rest any unfilled remainder (default, existing behavior)
constexpr char TIF_IOC = 'I';  // Immediate-Or-Cancel: match what's available, discard the remainder
constexpr char TIF_FOK = 'F';  // Fill-Or-Kill: fill completely or not at all, no partial fills

// Order lifecycle status.
constexpr uint8_t STATUS_OPEN = 0;
constexpr uint8_t STATUS_PARTIAL = 1;
constexpr uint8_t STATUS_FILLED = 2;
constexpr uint8_t STATUS_CANCELLED = 3;

// A single resting or incoming order.
//
// `prev`/`next` form an intrusive doubly-linked list. V1 (naive) ignores them;
// V2/V3 thread orders through price-level queues with them for O(1) append and
// O(1) cancel without any per-node container allocation.
//
// Prices are integers: price = dollars * 100 (price == 10050 means $100.50).
// No floating point anywhere in the matching path.
struct Order {
    uint64_t order_id = 0;
    uint64_t timestamp_ns = 0;
    char side = SIDE_BUY;       // 'B' or 'A'
    char type = TYPE_LIMIT;     // TYPE_LIMIT / TYPE_MARKET / TYPE_STOP / TYPE_STOP_LIMIT
    int64_t price = 0;          // integer ticks (dollars * 100); ignored for market/dormant-stop orders
    int64_t quantity = 0;       // original order size
    int64_t filled_qty = 0;     // cumulative filled so far
    uint8_t status = STATUS_OPEN;

    // Opaque trader/account identifier. 0 means "not participating in
    // self-trade prevention" -- this sentinel is what keeps every order that
    // never sets trader_id (the entire pre-existing test suite) behaving
    // exactly as before STP was added. See Engine/order-book STP checks.
    uint64_t trader_id = 0;

    // Time-in-force for TYPE_LIMIT orders (and the post-trigger Limit a
    // TYPE_STOP_LIMIT order converts into). Defaults to TIF_GTC, reproducing
    // today's exact "rest any remainder" behavior.
    char tif = TIF_GTC;

    // Trigger price for TYPE_STOP / TYPE_STOP_LIMIT orders; unused otherwise.
    int64_t stop_price = 0;

    // Intrusive list links (used by V2/V3).
    Order* prev = nullptr;
    Order* next = nullptr;

    // Remaining open quantity. Hot-path helper; trivially inlined.
    int64_t remaining() const { return quantity - filled_qty; }
};

}  // namespace me

#endif  // MATCHING_ENGINE_ORDER_HPP
