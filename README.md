# Order Book Matching Engine

A limit order book in C++17, the core of a stock exchange. It takes buy and sell orders, matches them by **price-time priority**, and reports trades. It supports limit, market and cancel orders. Orders arrive through a **multithreaded feed**: several producer threads push into a bounded queue, and a single matching thread owns the book. The repo also includes a benchmark for throughput and latency, a randomized test that checks every command against a simple reference implementation, and a concurrency test that runs under ThreadSanitizer. It uses only the standard library; GoogleTest is the one external dependency, and only for the tests.

## Features

- Limit orders: match immediately as far as possible, then rest in the book
- Market orders: match at any price; whatever is left over is cancelled and never rests
- Cancel by id in O(1) on average
- Validation: quantity ≤ 0, limit price ≤ 0 and duplicate ids are rejected with a reason. Cancels distinguish an unknown id from an order that is already filled or cancelled.
- Prices stored as integer ticks (0.01), parsed from decimal strings with no floating point
- Single-writer concurrency: many producer threads, one matching thread, no locks on the book
- CLI with interactive and replay modes, plus a `PRINT` depth view
- Benchmark with throughput, p50/p99/p99.9/max latency and a single-threaded baseline
- `checkInvariants()` runs after every step of every test

## Build and run

Requires CMake ≥ 3.16 and g++ or clang with C++17. CMake fetches GoogleTest automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

```bash
./build/engine                                # interactive, reads stdin
./build/engine --replay examples/orders.txt   # replay a file
./build/bench --orders 1000000 --producers 4 --seed 42
```

### Commands

```
ADD <id> BUY|SELL LIMIT <qty> <price>
ADD <id> BUY|SELL MARKET <qty>
CANCEL <id>
PRINT            # top 5 levels per side, best bid, best ask and spread
QUIT
```

Example (from `examples/orders.txt`):

```
ADD 6 BUY LIMIT 80 101.50
TRADE buy=6 sell=1 price=101.00 qty=50
TRADE buy=6 sell=3 price=101.00 qty=20
TRADE buy=6 sell=2 price=101.50 qty=10
ACCEPTED id=6 filled=80 resting=0

ADD 7 SELL MARKET 80
TRADE buy=4 sell=7 price=100.00 qty=40
TRADE buy=5 sell=7 price=99.50 qty=25
ACCEPTED id=7 filled=65 unfilled=15 (cancelled, market orders never rest)

CANCEL 1
REJECT id=1 reason=order_not_active

PRINT
------------ ORDER BOOK ------------
  side      price        qty  orders
  ASK      100.75         15       1
  ----------------------------------
  BID      100.25         15       1
best_bid=100.25 best_ask=100.75 spread=0.50
```

## Tests

```bash
cd build && ctest --output-on-failure
```

37 tests in three groups:

| File | What it covers |
|------|----------------|
| `tests/test_orderbook.cpp` | Empty book, resting, full and partial fills, sweeping several levels, price priority, time priority, trade price = resting price, market remainder cancelled, cancel (resting, unknown, already filled, twice), duplicate ids, invalid quantity/price, empty level removal, depth, price parsing |
| `tests/test_parser.cpp` | Command grammar, case-insensitivity, comments, malformed lines |
| `tests/test_randomized.cpp` | **100,000 random commands** (limits, markets, cancels, duplicate ids, invalid input) run through the engine **and** a deliberately naive reference book (one flat vector, scanned in full every time). Every report, every trade, best bid/ask and order count must match at every step. Plus 3 more seeds × 20,000. |
| `tests/test_concurrency.cpp` | The queue's blocking and close semantics. **4 producers × 25,000 orders** through the engine: every order is processed exactly once, and for every single order *filled + resting + cancelled = submitted*. Concurrent add/cancel producers and clean shutdown. |

Sanitizer builds:

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build-debug -j
cd build-debug && ctest

# ThreadSanitizer
cmake -S . -B build-tsan -DOB_TSAN=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-tsan -j
cd build-tsan && ctest
```

All 37 tests pass in Release, ASan+UBSan and TSan builds.

## Architecture

```
 producer 1 ─┐
 producer 2 ─┤  submit()    ┌─────────────────────┐   pop batch   ┌──────────────────────┐
 producer 3 ─┼────────────► │ OrderQueue<Command> │ ────────────► │ matching thread      │
 producer 4 ─┘  (blocks if  │ bounded, mutex + 2  │               │  owns the OrderBook  │──► trade / report /
                 full)      │ condition variables │               │  applies 1 at a time │    book listeners
                            └─────────────────────┘               └──────────────────────┘
```

| File | Role |
|------|------|
| `Types.hpp` | `Price`, `Quantity`, `OrderId`, `Sequence`, `Side`, `OrderType`; decimal ⇄ tick conversion |
| `Order.hpp`, `Trade.hpp` | Plain structs |
| `ExecutionReport.hpp` | Result of one operation: accepted/rejected, reason, trades, filled and remaining quantity |
| `OrderBook.hpp/.cpp` | `addLimit`, `addMarket`, `cancel`, `bestBid`, `bestAsk`, `depth(n)`, `checkInvariants` |
| `OrderQueue.hpp` | Bounded blocking queue: `push`, `pop`, `close` |
| `MatchingEngine.hpp/.cpp` | Owns the queue, the book and the matching thread; calls listeners |
| `CommandParser.hpp/.cpp` | Turns text lines into `Command`s |
| `src/main.cpp` | CLI |
| `bench/bench.cpp` | Benchmark |

### Data structures

```cpp
std::map<Price, PriceLevel, std::greater<Price>> bids_;   // begin() = best bid
std::map<Price, PriceLevel>                      asks_;   // begin() = best ask
struct PriceLevel { std::list<Order> orders; Quantity total; };
std::unordered_map<OrderId, Locator> index_;  // side + level iterator + list iterator
```

| Operation | Complexity |
|-----------|-----------|
| Add at a new price level | O(log L), where L = number of levels |
| Add at an existing level | O(log L) lookup + O(1) append |
| Cancel | O(1) average: the index holds iterators straight into the map and list |
| Match | O(k) for k fills (plus O(log L) per emptied level) |
| Best bid / ask | O(1) |

### Matching rules

1. **Buy limit at P**: while quantity remains and best ask ≤ P, fill against the oldest order at the best ask. The trade is at the ask price. Fully filled orders and empty levels are removed. The remainder rests at P.
2. **Sell limit**: mirror image, against bids ≥ P.
3. **Market**: match at any price until filled or the other side is empty. The remainder is cancelled and reported as `unfilled`.
4. **Cancel**: removes a resting order. Unknown ids → `unknown_id`. Filled or already-cancelled orders → `order_not_active`.
5. **Validation**: quantity ≤ 0, limit price ≤ 0, duplicate id → rejected. A rejected order does not use up its id.
6. **Invariant**: the book is never crossed. If both sides are non-empty, best bid < best ask.

## Benchmark results

`./build/bench --orders 1000000 --producers 4 --seed 42`. The order mix is 70% limit, 20% cancels, 10% market, around a mid of 100.00. Orders are generated before timing starts. Throughput covers the whole run, from the first push until the matching thread has drained the queue. Latency is measured on the matching thread with `std::chrono::steady_clock`, from when a command is taken off the queue to when it finishes processing.

**Machine:** Intel Xeon Processor @ 2.10GHz, 4 vCPUs (shared cloud VM), Ubuntu 24.04, g++ 13.3.0, `-O2 -DNDEBUG`. Median of 3 runs:

| Mode | Throughput | p50 | p99 | p99.9 |
|------|-----------:|----:|----:|------:|
| 4 producers → queue → 1 matching thread | ~81,000 orders/s | 0.69 µs | 3.8 µs | 13 µs |
| Single-threaded baseline (direct calls, no queue) | **~1.6 M orders/s** | 0.33 µs | 1.8 µs | 5.1 µs |

The max latency (tens of ms) is not in the table. It comes from rare OS scheduling or page-fault stalls on the shared VM, not from the book.



## Design decisions and trade-offs

- **Integer prices.** Floating-point rounding makes `==` and `<` on prices unreliable, and an order book is built on exactly those comparisons. Prices are `int64_t` ticks, parsed straight from the decimal string.
- **`std::map`, not `std::priority_queue`.** A heap only exposes its top. The book has to walk levels in order (for sweeps and `depth`) and remove arbitrary orders (cancels).
- **`std::list` inside a level, not `std::vector`.** List iterators stay valid when other elements are inserted or erased, so the index can point straight at an order and a cancel is O(1). The cost is worse cache locality while matching.
- **One matching thread instead of locking the book.** Processing order is deterministic and fair (one global arrival sequence), the hot path has no lock contention, and the book code stays simple. Real exchanges do the same.
- **Bounded queue.** If the matching thread falls behind, producers block (back-pressure) instead of memory growing without limit.
- **Clean shutdown.** `stop()` closes the queue. Producers can no longer push, the matching thread drains what is left, then exits and is joined.
- **Trade price = resting order's price.** The resting order was there first and set the price; the incoming order accepted it by crossing.
- **Ids stay used forever.** The book keeps a set of every accepted id, so a duplicate is caught even after the original order is gone. It also lets cancel tell `unknown_id` apart from `order_not_active`.

### How to make it faster

- Object pool for orders, so there is no allocation per order
- Intrusive linked lists, or a flat array indexed by price tick around the mid
- Lock-free single-producer single-consumer ring buffer (`std::atomic`) per producer
- Avoid `std::function` listeners on the hot path

## License

MIT, see [LICENSE](LICENSE).
