# Extended Order Types + Self-Trade Prevention Implementation Plan


**Goal:** Add IOC/FOK/GTC time-in-force, Stop/Stop-Limit order types, and cancel-resting self-trade prevention to all three interchangeable order-book engines (V1 naive, V2/V3 optimized), without changing behavior for any existing default (GTC, non-STP, non-stop) order.

**Architecture:** `Order` gains three new default-initialized fields (`trader_id`, `tif`, `stop_price`) so every existing call site keeps compiling and behaving identically. `IOrderBook` gains one new pure-virtual method, `can_fill_completely`, implemented by both engines as a non-mutating liquidity scan (used for FOK). TIF and self-trade prevention are implemented inside each engine's existing `add_limit_order`/`match` methods. Stop and Stop-Limit orders never enter the live book — they rest in a new dormant FIFO structure owned by `Engine` (not `IOrderBook`) and are triggered by `Engine::submit_order` after each trade-producing operation, converting to Market/Limit and re-dispatching through the same book, looping until no more stops trigger (handles cascades).

**Tech Stack:** C++17, CMake, GoogleTest (parameterized `TEST_P` over `BookKind`).

## Global Constraints

- All 153 currently-passing tests (17 bodies x 3 `INSTANTIATE_TEST_SUITE_P` registrations x 3 `BookKind` values) must continue passing unchanged after every task — verify with `ctest --test-dir build --output-on-failure` (or the direct binary) after each implementation task.
- `Order::trader_id` defaults to `0`; self-trade prevention only activates when `order->trader_id != 0 && maker->trader_id == order->trader_id` — this is what keeps every pre-existing call site (which never sets `trader_id`, defaulting to `0` on both sides) behaving exactly as before. Document this sentinel behavior inline wherever STP is implemented.
- `Order::tif` defaults to `TIF_GTC`, reproducing today's exact resting behavior for any order that doesn't opt into IOC/FOK.
- Stop/Stop-Limit orders are handled entirely by `Engine`, never by `IOrderBook` implementations — V1 and V2/V3 matching cores never see `TYPE_STOP`/`TYPE_STOP_LIMIT` order types.
- New tests are added as `TEST_P` bodies in the existing shared `OrderBookTest` fixture pattern (`tests/test_helpers.hpp`), each run against all 3 `BookKind` values via `INSTANTIATE_TEST_SUITE_P`.
- Full spec: `docs/design/2026-07-10-extended-order-types-design.md`.

---

### Task 1: `Order` struct fields and new type/TIF constants

**Files:**
- Modify: `include/order.hpp`

**Interfaces:**
- Produces: `constexpr char TYPE_STOP = 'S'`, `constexpr char TYPE_STOP_LIMIT = 'P'`, `constexpr char TIF_GTC = 'G'`, `constexpr char TIF_IOC = 'I'`, `constexpr char TIF_FOK = 'F'`, and on `Order`: `uint64_t trader_id = 0`, `char tif = TIF_GTC`, `int64_t stop_price = 0`. All later tasks depend on these exact names and default values.

- [ ] **Step 1: Add the new constants and fields**

Replace the full contents of `include/order.hpp` with:

```cpp
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
```

- [ ] **Step 2: Build to confirm nothing broke**

Run:
```bash
source .venv/bin/activate
cmake --build build -j"$(nproc)"
```
Expected: builds cleanly (no errors). New fields are additive and default-initialized, so every existing `.cpp` file compiles unchanged.

- [ ] **Step 3: Run the full existing suite to confirm zero regressions**

Run: `./build/matching_engine_tests`
Expected: `[==========] 153 tests from ... ran.` and `[  PASSED  ] 153 tests.`

- [ ] **Step 4: Commit**

```bash
git add include/order.hpp
git commit -m "feat: add trader_id/tif/stop_price fields and new order type/TIF constants"
```

---

### Task 2: `IOrderBook::can_fill_completely` interface method

**Files:**
- Modify: `include/order_book.hpp`

**Interfaces:**
- Consumes: `Order` (Task 1).
- Produces: `virtual bool can_fill_completely(const Order* order) const = 0;` on `IOrderBook` — Tasks 4 and 5 must each provide a concrete override with this exact signature, or the build fails (both concrete classes are `final` and must implement every pure virtual).

- [ ] **Step 1: Add the pure-virtual method**

In `include/order_book.hpp`, add this method to `IOrderBook`, directly after `modify_order`'s declaration (before `best_bid`):

```cpp
    // Non-mutating check: would submitting `order` as a limit order fill it
    // completely against current opposite-side liquidity (honoring its limit
    // price), leaving no resting remainder? Used to implement Fill-Or-Kill
    // (TIF_FOK) without ever partially mutating book state on failure. Returns
    // false for non-TYPE_LIMIT orders (FOK only applies to limit orders). Does
    // NOT account for self-trade prevention -- a same-trader resting order
    // still counts as available liquidity for this check; STP's cancellation
    // of that order happens only during the real, mutating match.
    virtual bool can_fill_completely(const Order* order) const = 0;
```

- [ ] **Step 2: Confirm the build fails as expected (interface has no implementers yet)**

Run:
```bash
cmake --build build -j"$(nproc)" 2>&1 | tail -30
```
Expected: FAIL — compiler errors that `NaiveOrderBook`/`OptimizedOrderBook` are abstract because `can_fill_completely` is not overridden (e.g. `cannot declare variable ... to be of abstract type`). This confirms the interface change is wired up; Tasks 4 and 5 will fix the build.

- [ ] **Step 3: Commit**

```bash
git add include/order_book.hpp
git commit -m "feat: add can_fill_completely pure-virtual method to IOrderBook for FOK support"
```

---

### Task 3: Extend the shared test helper `mk()` with trader_id/tif/stop_price

**Files:**
- Modify: `tests/test_helpers.hpp`

**Interfaces:**
- Consumes: `Order` fields from Task 1.
- Produces: `Order* mk(char side, char type, int64_t qty, int64_t price = 0, uint64_t trader_id = 0, char tif = TIF_GTC, int64_t stop_price = 0)` — Task 6 and Task 8 test bodies call this with all 7 arguments.

- [ ] **Step 1: Extend `mk()`'s signature and body**

In `tests/test_helpers.hpp`, replace the `mk` method body with:

```cpp
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
```

- [ ] **Step 2: Confirm the build still fails only on the two known-missing overrides**

Run:
```bash
cmake --build build -j"$(nproc)" 2>&1 | tail -30
```
Expected: same abstract-class errors as Task 2 Step 2 (this change only touches a test header; it doesn't fix or worsen the interface gap). `matching_engine_core` itself (the library, not the test binary) should still fail to build `NaiveOrderBook`/`OptimizedOrderBook` instantiation sites.

- [ ] **Step 3: Commit**

```bash
git add tests/test_helpers.hpp
git commit -m "test: extend mk() helper with trader_id/tif/stop_price parameters"
```

---

### Task 4: V1 naive engine — FOK check, IOC/FOK dispatch, self-trade prevention

**Files:**
- Modify: `src/naive/order_book.cpp`

**Interfaces:**
- Consumes: `Order::trader_id/tif/stop_price` (Task 1), `IOrderBook::can_fill_completely` (Task 2).
- Produces: `NaiveOrderBook::can_fill_completely` override; modified `NaiveOrderBook::add_limit_order` (TIF-aware); modified `NaiveOrderBook::match` (STP-aware). No signature changes visible outside this file.

- [ ] **Step 1: Declare `can_fill_completely` in the class**

In `src/naive/order_book.cpp`, in the `NaiveOrderBook` public section, add the override declaration directly after `modify_order`:

```cpp
    bool can_fill_completely(const Order* order) const override;
```

- [ ] **Step 2: Implement the non-mutating FOK liquidity scan**

Add this method definition after `NaiveOrderBook::best_ask` (before `NaiveOrderBook::rest`):

```cpp
bool NaiveOrderBook::can_fill_completely(const Order* order) const {
    if (order->type != TYPE_LIMIT) return false;  // FOK only defined for limit orders
    const bool buying = (order->side == SIDE_BUY);
    const std::map<int64_t, std::queue<Slot>>& opp = buying ? asks_ : bids_;
    const int64_t needed = order->remaining();
    int64_t available = 0;

    if (buying) {
        for (auto it = opp.begin(); it != opp.end(); ++it) {
            if (it->first > order->price) break;  // no longer crosses
            std::queue<Slot> copy = it->second;
            while (!copy.empty()) {
                if (slot_live(copy.front())) available += copy.front().order->remaining();
                copy.pop();
            }
            if (available >= needed) return true;
        }
    } else {
        for (auto it = opp.rbegin(); it != opp.rend(); ++it) {
            if (it->first < order->price) break;  // no longer crosses
            std::queue<Slot> copy = it->second;
            while (!copy.empty()) {
                if (slot_live(copy.front())) available += copy.front().order->remaining();
                copy.pop();
            }
            if (available >= needed) return true;
        }
    }
    return available >= needed;
}
```

- [ ] **Step 3: Add self-trade prevention to the matching loop**

In `NaiveOrderBook::match`, find this inner loop:

```cpp
        std::queue<Slot>& q = best_it->second;
        while (order->remaining() > 0 && !q.empty()) {
            Slot front = q.front();
            if (!slot_live(front)) {
                q.pop();
                continue;
            }
            Order* maker = front.order;
            const int64_t fill = std::min(order->remaining(), maker->remaining());
```

Replace it with:

```cpp
        std::queue<Slot>& q = best_it->second;
        while (order->remaining() > 0 && !q.empty()) {
            Slot front = q.front();
            if (!slot_live(front)) {
                q.pop();
                continue;
            }
            Order* maker = front.order;

            if (order->trader_id != 0 && maker->trader_id == order->trader_id) {
                // Self-trade prevention (cancel-resting mode): trader_id == 0
                // means "not participating in STP" (the default for every
                // pre-existing order), so this never fires unless both sides
                // explicitly opted in with a matching nonzero trader_id.
                maker->status = STATUS_CANCELLED;
                ++epoch_[maker->order_id];
                order_index_.erase(maker->order_id);
                q.pop();
                continue;
            }

            const int64_t fill = std::min(order->remaining(), maker->remaining());
```

- [ ] **Step 4: Add IOC/FOK dispatch to `add_limit_order`**

Replace `NaiveOrderBook::add_limit_order`:

```cpp
std::vector<Trade> NaiveOrderBook::add_limit_order(Order* order) {
    order->status = STATUS_OPEN;

    if (order->tif == TIF_FOK && !can_fill_completely(order)) {
        return {};  // Fill-Or-Kill: cannot fill in full, no trades, no mutation.
    }

    std::vector<Trade> trades = match(order, /*is_market=*/false);

    if (order->remaining() > 0) {
        order->status = (order->filled_qty > 0) ? STATUS_PARTIAL : STATUS_OPEN;
        if (order->tif == TIF_GTC) {
            rest(order);
        }
        // TIF_IOC (and the FOK safety-net above) discard any remainder
        // instead of resting it.
    } else {
        order->status = STATUS_FILLED;
    }
    return trades;
}
```

- [ ] **Step 5: Build (V1 side of the interface gap is now fixed; V2/V3 still missing until Task 5)**

Run:
```bash
cmake --build build -j"$(nproc)" 2>&1 | tail -40
```
Expected: `OptimizedOrderBook` still reported abstract/missing `can_fill_completely` (Task 5 not done yet); no errors related to `NaiveOrderBook` or `src/naive/order_book.cpp`.

- [ ] **Step 6: Commit**

```bash
git add src/naive/order_book.cpp
git commit -m "feat: add FOK check, IOC/FOK dispatch, and self-trade prevention to V1 naive engine"
```

---

### Task 5: V2/V3 optimized engine — FOK check, IOC/FOK dispatch, self-trade prevention

**Files:**
- Modify: `src/optimized/order_book.cpp`

**Interfaces:**
- Consumes: `Order::trader_id/tif/stop_price` (Task 1), `IOrderBook::can_fill_completely` (Task 2). Independent of Task 4 (different file, same pattern applied to a different class) — safe to implement in parallel.
- Produces: `OptimizedOrderBook::can_fill_completely` override; modified `OptimizedOrderBook::add_limit_order` (TIF-aware); modified `OptimizedOrderBook::match` (STP-aware).

- [ ] **Step 1: Declare `can_fill_completely` in the class**

In `src/optimized/order_book.cpp`, in the `OptimizedOrderBook` public section, add directly after `modify_order`:

```cpp
    bool can_fill_completely(const Order* order) const override;
```

- [ ] **Step 2: Implement the non-mutating FOK liquidity scan using per-level `total_qty`**

Add this method definition after `OptimizedOrderBook::best_ask` (the last method before the factory function):

```cpp
bool OptimizedOrderBook::can_fill_completely(const Order* order) const {
    if (order->type != TYPE_LIMIT) return false;  // FOK only defined for limit orders
    const bool buying = (order->side == SIDE_BUY);
    const std::vector<Level>& opp_levels = buying ? ask_levels_ : bid_levels_;
    const int64_t opp_best = buying ? best_ask_idx_ : best_bid_idx_;
    const int64_t needed = order->remaining();
    int64_t available = 0;

    if (opp_best == -1) return false;

    if (buying) {
        for (int64_t i = opp_best; static_cast<size_t>(i) < opp_levels.size(); ++i) {
            const int64_t level_price = min_price_ + i * tick_;
            if (level_price > order->price) break;  // no longer crosses
            available += opp_levels[static_cast<size_t>(i)].total_qty;
            if (available >= needed) return true;
        }
    } else {
        for (int64_t i = opp_best; i >= 0; --i) {
            const int64_t level_price = min_price_ + i * tick_;
            if (level_price < order->price) break;  // no longer crosses
            available += opp_levels[static_cast<size_t>(i)].total_qty;
            if (available >= needed) return true;
        }
    }
    return available >= needed;
}
```

- [ ] **Step 3: Add self-trade prevention to the matching loop**

In `OptimizedOrderBook::match`, find this inner loop:

```cpp
        while (order->remaining() > 0 && !level.empty()) {
            Order* maker = level.head;
            const int64_t fill = std::min(order->remaining(), maker->remaining());
```

Replace it with:

```cpp
        while (order->remaining() > 0 && !level.empty()) {
            Order* maker = level.head;

            if (order->trader_id != 0 && maker->trader_id == order->trader_id) {
                // Self-trade prevention (cancel-resting mode): trader_id == 0
                // means "not participating in STP" (the default for every
                // pre-existing order), so this never fires unless both sides
                // explicitly opted in with a matching nonzero trader_id.
                level.total_qty -= maker->remaining();
                remove_node(level, maker);
                maker->status = STATUS_CANCELLED;
                order_index_.erase(maker->order_id);
                continue;
            }

            const int64_t fill = std::min(order->remaining(), maker->remaining());
```

- [ ] **Step 4: Add IOC/FOK dispatch to `add_limit_order`**

Replace `OptimizedOrderBook::add_limit_order`:

```cpp
std::vector<Trade> OptimizedOrderBook::add_limit_order(Order* order) {
    order->status = STATUS_OPEN;

    if (order->tif == TIF_FOK && !can_fill_completely(order)) {
        return {};  // Fill-Or-Kill: cannot fill in full, no trades, no mutation.
    }

    std::vector<Trade> trades = match(order, /*is_market=*/false);

    if (order->remaining() > 0) {
        order->status = (order->filled_qty > 0) ? STATUS_PARTIAL : STATUS_OPEN;
        if (order->tif == TIF_GTC) {
            rest(order);
        }
        // TIF_IOC (and the FOK safety-net above) discard any remainder
        // instead of resting it.
    } else {
        order->status = STATUS_FILLED;
    }
    return trades;
}
```

- [ ] **Step 5: Build — the interface gap from Task 2 is now fully closed**

Run:
```bash
cmake --build build -j"$(nproc)"
```
Expected: clean build, all 3 targets (`matching_engine_core`, `matching_engine_tests`, `matching_engine_benchmarks`) compile successfully.

- [ ] **Step 6: Run the full existing suite to confirm zero regressions**

Run: `./build/matching_engine_tests`
Expected: `[  PASSED  ] 153 tests.` — every default-GTC, non-STP, non-stop test path is completely unaffected by the new TIF/STP/FOK code paths.

- [ ] **Step 7: Commit**

```bash
git add src/optimized/order_book.cpp
git commit -m "feat: add FOK check, IOC/FOK dispatch, and self-trade prevention to V2/V3 optimized engine"
```

---

### Task 6: IOC/FOK/STP tests

**Files:**
- Create: `tests/test_order_types.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `mk()` (Task 3), `NaiveOrderBook`/`OptimizedOrderBook` TIF+STP behavior (Tasks 4, 5), `TIF_IOC`/`TIF_FOK`/`STATUS_CANCELLED` (Task 1).

- [ ] **Step 1: Register the new test file in the build**

In `CMakeLists.txt`, find:

```cmake
add_executable(matching_engine_tests
  tests/test_matching.cpp
  tests/test_priorities.cpp
  tests/test_edge_cases.cpp
)
```

Replace with:

```cmake
add_executable(matching_engine_tests
  tests/test_matching.cpp
  tests/test_priorities.cpp
  tests/test_edge_cases.cpp
  tests/test_order_types.cpp
)
```

- [ ] **Step 2: Write the IOC/FOK/STP test bodies**

Create `tests/test_order_types.cpp`:

```cpp
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
```

- [ ] **Step 3: Reconfigure and build**

Run:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```
Expected: clean build (the new file is picked up by CMake's `FetchContent`-based `gtest_discover_tests`).

- [ ] **Step 4: Run the tests**

Run: `./build/matching_engine_tests`
Expected: all previous 153 tests still pass, plus 5 new bodies x 3 `BookKind` = 15 new passing tests (168 total).

- [ ] **Step 5: Commit**

```bash
git add tests/test_order_types.cpp CMakeLists.txt
git commit -m "test: add IOC/FOK/STP parameterized test coverage across all 3 engines"
```

---

### Task 7: Engine-level dormant stop-order tracking and trigger logic

**Files:**
- Modify: `include/engine.hpp`
- Modify: `src/engine.cpp`

**Interfaces:**
- Consumes: `Order::type/side/stop_price` (Task 1), `IOrderBook::add_limit_order`/`add_market_order` (unchanged signatures, now TIF/STP-aware per Tasks 4/5).
- Produces: `std::vector<Trade> Engine::submit_order(Order* order)` — Task 8 test bodies call this (not `engine_->book().add_limit_order(...)` directly) for every order that might be a stop, or that might trigger one.

- [ ] **Step 1: Add `submit_order` and private trigger-loop members to `Engine`**

In `include/engine.hpp`, replace the `Engine` class body:

```cpp
class Engine {
public:
    explicit Engine(BookKind kind, int64_t min_price = 0, int64_t tick = 1)
        : book_(make_order_book(kind, min_price, tick)) {}

    IOrderBook& book() { return *book_; }
    const IOrderBook& book() const { return *book_; }

    // Monotonic id / timestamp helpers (timestamps are synthetic, monotonically
    // increasing — deterministic and floating-point-free).
    uint64_t next_order_id() { return ++order_id_; }
    uint64_t next_timestamp() { return ++clock_ns_; }

    // Submit a Limit, Market, Stop, or Stop-Limit order.
    //
    // Limit/Market orders go straight to the book. Stop/Stop-Limit orders never
    // touch the live book -- they rest dormant (FIFO per side, by placement
    // order) until a trade prints at a price that crosses their stop_price:
    // buy stops trigger when trade price >= stop_price, sell stops trigger
    // when trade price <= stop_price. On trigger, TYPE_STOP converts to
    // TYPE_MARKET and TYPE_STOP_LIMIT converts to TYPE_LIMIT (keeping its
    // original price/tif), then it is dispatched to the book like any other
    // order -- whose own resulting trade can trigger further stops, handled
    // as a loop until quiescent (cascading triggers).
    //
    // Returns every trade produced, including from cascaded triggers, in the
    // order they occurred.
    std::vector<Trade> submit_order(Order* order);

private:
    std::vector<Trade> dispatch_to_book(Order* order);
    void trigger_ready_stops(int64_t last_trade_price, std::vector<Trade>& out_trades);
    Order* pop_triggerable_buy_stop(int64_t last_trade_price);
    Order* pop_triggerable_sell_stop(int64_t last_trade_price);

    std::unique_ptr<IOrderBook> book_;
    uint64_t order_id_ = 0;
    uint64_t clock_ns_ = 0;
    std::vector<Order*> dormant_buy_stops_;   // FIFO by placement order
    std::vector<Order*> dormant_sell_stops_;  // FIFO by placement order
};
```

- [ ] **Step 2: Implement `submit_order` and its helpers in `src/engine.cpp`**

Replace the full contents of `src/engine.cpp` with:

```cpp
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
```

- [ ] **Step 3: Build**

Run:
```bash
cmake --build build -j"$(nproc)"
```
Expected: clean build.

- [ ] **Step 4: Run the full existing suite to confirm zero regressions**

Run: `./build/matching_engine_tests`
Expected: `[  PASSED  ] 168 tests.` (153 original + 15 from Task 6) — `Engine::submit_order` is new and additive; nothing existing calls it yet.

- [ ] **Step 5: Commit**

```bash
git add include/engine.hpp src/engine.cpp
git commit -m "feat: add Engine::submit_order with dormant stop-order tracking and cascade-triggering"
```

---

### Task 8: Stop / Stop-Limit / cascading-stop tests

**Files:**
- Modify: `tests/test_order_types.cpp`

**Interfaces:**
- Consumes: `Engine::submit_order` (Task 7), `mk()` (Task 3), `TYPE_STOP`/`TYPE_STOP_LIMIT` (Task 1).

- [ ] **Step 1: Append stop-order test bodies**

In `tests/test_order_types.cpp`, insert the following `TEST_P` bodies directly before the `INSTANTIATE_TEST_SUITE_P(OrderTypeTests, ...)` line added in Task 6:

```cpp
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
```

- [ ] **Step 2: Build**

Run:
```bash
cmake --build build -j"$(nproc)"
```
Expected: clean build.

- [ ] **Step 3: Run the full suite**

Run: `./build/matching_engine_tests`
Expected: `[  PASSED  ] 177 tests.` (168 from Task 7 + 3 new bodies x 3 `BookKind` = 9 new).

- [ ] **Step 4: Run with sanitizers to confirm no memory-safety regressions in the new pointer manipulation (STP cancellation, intrusive-list removal, dormant-vector erase)**

Run:
```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j"$(nproc)"
./build-asan/matching_engine_tests
```
Expected: `[  PASSED  ] 177 tests.` with no sanitizer errors printed.

- [ ] **Step 5: Commit**

```bash
git add tests/test_order_types.cpp
git commit -m "test: add Stop/Stop-Limit/cascading-stop parameterized test coverage across all 3 engines"
```

---

## Final Verification

- [ ] Run the full release build + benchmark suite once more to confirm nothing else broke:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/matching_engine_tests
```
Expected: `[  PASSED  ] 177 tests.`
- [ ] Update `README.md`'s test count (153 -> 177) and add a one-line mention of IOC/FOK/GTC, Stop/Stop-Limit, and self-trade prevention under "Architecture" or a new "Order types" bullet, matching the file's existing documentation style.
- [ ] Update the study material PDF (`study_material/Quant_SDE/cpp_matching_engine/study.tex`, two directories up from this repo) to move Section 9 ("Roadmap") from "in progress" to reporting the real, final test count and a short summary of what phase 1 actually shipped, then recompile with `latexmk -pdf -interaction=nonstopmode study.tex` from that directory.
