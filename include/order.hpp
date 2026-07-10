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
    char type = TYPE_LIMIT;     // 'L' or 'M'
    int64_t price = 0;          // integer ticks (dollars * 100); ignored for market orders
    int64_t quantity = 0;       // original order size
    int64_t filled_qty = 0;     // cumulative filled so far
    uint8_t status = STATUS_OPEN;

    // Intrusive list links (used by V2/V3).
    Order* prev = nullptr;
    Order* next = nullptr;

    // Remaining open quantity. Hot-path helper; trivially inlined.
    int64_t remaining() const { return quantity - filled_qty; }
};

}  // namespace me

#endif  // MATCHING_ENGINE_ORDER_HPP
