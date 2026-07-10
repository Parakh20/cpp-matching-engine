# Extended Order Types + Self-Trade Prevention — Design

## Context

The engine currently supports two order types (`TYPE_LIMIT`, `TYPE_MARKET`)
across three interchangeable book implementations (V1 naive, V2 optimized,
V3 arena-allocated), all behind the `IOrderBook` interface and validated by
a shared parameterized GoogleTest suite. This is phase 1 of a larger,
explicitly decomposed roadmap (see conversation) toward a more
production-grade exchange core. Later phases (WAL/recovery, market data
feed, risk engine, multi-threaded pipeline, etc.) are out of scope here and
will get their own spec.

## Goals

- Add time-in-force variants to limit orders: GTC (default, current
  behavior), IOC, FOK.
- Add Stop and Stop-Limit order types that rest dormant until triggered by
  trade price, then convert to Market / Limit respectively.
- Add self-trade prevention (STP), cancel-resting mode: an incoming order
  never trades against a resting order from the same trader.
- Preserve the existing invariant: all three engine versions (V1/V2/V3)
  implement identical behavior, verified by the same parameterized test
  suite.

## Non-goals

- No new order types beyond IOC/FOK/GTC/Stop/Stop-Limit (no Iceberg,
  Pegged, Trailing Stop, Auction — later phases if pursued).
- No multi-trader/account management system — `trader_id` is an opaque
  `uint64_t` the caller assigns; no session/auth concept.
- No change to price/time priority semantics for GTC orders.
- No persistence/WAL — that's a separate phase.

## Data model changes

`include/order.hpp`:

- Add `uint64_t trader_id = 0`.
- Add `char tif = 'G'` with constants `TIF_GTC = 'G'`, `TIF_IOC = 'I'`,
  `TIF_FOK = 'F'`.
- Add `constexpr char TYPE_STOP = 'S'` and `constexpr char TYPE_STOP_LIMIT
  = 'P'`.
- Add `int64_t stop_price = 0` (trigger price; unused for non-stop types).

All additions are default-initialized so existing call sites that only set
`side`/`type`/`price`/`quantity` keep compiling and behave exactly as
before (default `tif = TIF_GTC` reproduces current behavior).

## Behavior

### Time-in-force (limit orders only)

- **GTC**: unchanged — match against the opposite side as far as price
  allows, rest any remainder.
- **IOC**: match as far as price allows, then discard any remainder
  (never rests). Implemented as: run the existing match loop, then skip
  the "insert remainder into book" step when `tif == TIF_IOC`.
- **FOK**: the order must fill completely or not at all, with no partial
  fills and no state mutation on failure. Requires a non-mutating
  "can this fully fill" scan: sum available opposite-side liquidity at
  price levels crossable by the incoming order; if `>= quantity`, proceed
  with the normal match (guaranteed to fully fill); otherwise return no
  trades and leave the book untouched. Each of V1/V2/V3 needs this scan
  added since none currently support look-ahead without committing.

Market orders remain IOC-like by construction (already never rest); no
change needed there beyond STP (below).

### Self-trade prevention (cancel-resting mode)

During matching (for both limit and market incoming orders), when the
next eligible resting order at the current price level has the same
`trader_id` as the incoming order:

1. Remove that resting order from the book (cancel, not a trade).
2. Continue matching at the next resting order (same level, then next
   level), without consuming any of the incoming order's quantity against
   it.

This applies uniformly across V1/V2/V3 matching loops.

### Stop / Stop-Limit orders

Stop orders never enter the live matching book directly — they rest in a
new dormant per-side structure owned by `Engine`, not `IOrderBook`, so
they cannot affect `best_bid()`/`best_ask()` or be matched against until
triggered.

- **Trigger condition**: checked by `Engine` after every trade-producing
  operation, using the last trade price. Buy-side stops (`side ==
  SIDE_BUY`) trigger when trade price `>= stop_price`. Sell-side stops
  trigger when trade price `<= stop_price`.
- **On trigger**:
  - `TYPE_STOP` converts to a `TYPE_MARKET` order (using the stop order's
    remaining quantity) and is submitted via `add_market_order`.
  - `TYPE_STOP_LIMIT` converts to a `TYPE_LIMIT` order at its `price`,
    honoring its original `tif`, and is submitted via `add_limit_order`.
- Triggering is a `Engine`-level responsibility, not an `IOrderBook`
  concern, so V1/V2/V3 matching cores need no changes to support stop
  orders beyond accepting the converted Market/Limit order through their
  existing entry points.
- Multiple stops triggered by the same trade fire in the order they were
  placed (FIFO per side), each potentially producing further trades that
  can cascade further stop triggers — processed as a queue until no more
  stops trigger.

## Interface impact

`IOrderBook` (`include/order_book.hpp`):

- `add_limit_order` / `add_market_order` signatures unchanged — TIF and
  STP data travel on the `Order*` already passed in.
- New pure-virtual method for the FOK dry-run check, e.g.:
  `virtual bool can_fill_completely(const Order* order) const = 0;`
  implemented by each of V1/V2/V3 as a non-mutating liquidity scan.

`Engine` (`include/engine.hpp`, `src/engine.cpp`):

- Gains ownership of the dormant stop-order structure (e.g. `std::vector`
  or side-indexed list of pending stop `Order*`, plus the trader_id-aware
  cancel path for STP is entirely inside the book implementations).
- Gains a `submit_order` or equivalent entry point that runs the
  Market/Limit order through the book, then checks/fires any newly
  triggered stops, looping until quiescent.

## Testing

New `TEST_P` cases in the existing parameterized fixture
(`tests/test_helpers.hpp`), each run against all 3 `BookKind`s:

- IOC: partial fill against thin liquidity, remainder discarded, book
  ends without the resting IOC leftover.
- FOK: succeeds when liquidity fully covers the order (single trade,
  full fill). Fails-safe when liquidity insufficient (zero trades, book
  state unchanged, verified via `best_bid`/`best_ask` before/after).
- STP: incoming order same `trader_id` as best resting order — resting
  order cancelled (not traded), incoming order matches through to the
  next eligible resting order or rests/discards per its own `tif`.
- Stop order: rests dormant (does not affect `best_bid`/`best_ask`),
  triggers on qualifying trade price, converts to market and matches.
- Stop-limit order: rests dormant, triggers on qualifying trade price,
  converts to limit order and either matches or rests per normal limit
  semantics.
- Cascading stops: one trade triggers a stop whose resulting trade
  triggers a second stop.

Existing 51-case x 3-engine suite must continue passing unchanged (proves
the new fields/behavior are additive, not breaking, for default
GTC/non-STP/non-stop orders).
