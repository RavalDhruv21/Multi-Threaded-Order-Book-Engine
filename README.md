# hft-orderbook-cpp

A low-latency, multi-threaded limit order book engine in C++20, built to
demonstrate the systems-engineering techniques used in real HFT matching
engines: zero-allocation hot path, lock-free SPSC message passing, cache-line
aware data layout, and O(1) price-time-priority matching.

This is a portfolio/interview project, not a production trading system —
but every design decision below is one a real venue actually makes, not a
simplification hand-waved away.

## Why this isn't "just another order book on GitHub"

Most order-book toy projects reach for `std::map<Price, std::list<Order>>`
and call it a day. That's O(log n) inserts with pointer-chasing and heap
churn on every single order — the opposite of what a matching engine needs.
Three things here are meant to actually hold up under scrutiny:

1. **O(1) best-bid/best-ask via a two-level summary bitmap**, not a
   `std::map` and not a linear scan. See [`SummaryBitmap`](include/OrderBook.hpp)
   in `OrderBook.hpp` — a 64-bit "summary" word tells you in one branch
   whether any of the 64 underlying 64-bit level words has a bit set, and
   `std::countr_zero`/`std::countl_zero` (single hardware instructions —
   `tzcnt`/`lzcnt`) find the exact level. Two loads, two intrinsics, done.
2. **Generation-counted order handles**, not raw pool indices. `OrderId`
   packs a slot index *and* a generation counter (`Types.hpp`). Pool slots
   are recycled the instant an order is filled or cancelled; without a
   generation check, a client holding a stale `OrderId` could silently
   cancel or corrupt a *different* order that reused the same memory (the
   classic ABA problem). This is checked in [`test_stale_handle_after_slot_reuse_is_rejected`](tests/test_orderbook.cpp).
3. **Exchange-accurate amend semantics**: a quantity-only decrease at the
   same price mutates in place and *keeps* time priority; a price change or
   quantity increase is cancel-replace and *loses* it. That's how
   Nasdaq/CME actually behave, and it's a common thing candidates get wrong
   by treating "amend" as "just edit the fields."

## Architecture

```
                    ┌─────────────────────┐
   Producer thread  │  random order        │
   (market data      │  generator            │
   simulator)        └──────────┬───────────┘
                                 │ push()
                                 ▼
                    ┌─────────────────────┐
                    │  SPSCQueue<T,N>      │   lock-free ring buffer
                    │  (SPSCQueue.hpp)     │   atomic head/tail,
                    └──────────┬───────────┘   acquire/release only
                                 │ pop()
                                 ▼
                    ┌─────────────────────┐
   Consumer thread  │  OrderBook           │
   (matching        │  match_or_add()      │
   engine)          └──────────┬───────────┘
                                 │
                 ┌───────────────┴────────────────┐
                 ▼                                  ▼
      ┌─────────────────────┐          ┌─────────────────────┐
      │ MemoryPool<Order,N>  │          │ SummaryBitmap x2      │
      │ (MemoryPool.hpp)     │          │ (bids_ / asks_)       │
      │ array + free-list,   │          │ O(1) best-price       │
      │ zero heap traffic    │          │ discovery              │
      └─────────────────────┘          └─────────────────────┘
```

### Files

| File | Responsibility |
|---|---|
| [`include/Types.hpp`](include/Types.hpp) | Strong-ish domain types, the `OrderHandle` generation-counter encoding, wire-format `IncomingOrder` / `ExecReport` structs. |
| [`include/MemoryPool.hpp`](include/MemoryPool.hpp) | Fixed-capacity slab allocator: `acquire()`/`release()` are O(1) free-list push/pop. No `new`/`delete`/`malloc` after startup. |
| [`include/SPSCQueue.hpp`](include/SPSCQueue.hpp) | Lock-free single-producer/single-consumer ring buffer. `std::atomic<size_t>` head/tail, cache-line padded, `acquire`/`release` ordering only (no `seq_cst`, no mutex). |
| [`include/OrderBook.hpp`](include/OrderBook.hpp) | The engine: `SummaryBitmap`, the cache-line-sized intrusive `Order` node, `PriceLevel`, and `OrderBook::match_or_add / cancel_order / amend_order`. |
| [`src/main.cpp`](src/main.cpp) | Two-thread benchmark harness: producer floods 100,000 random crossing orders through the queue, consumer times every `match_or_add()` call and reports mean/p50/p90/p99/p99.9/max latency. |
| [`tests/test_orderbook.cpp`](tests/test_orderbook.cpp) | Dependency-free functional tests: price-time priority, partial fills, market-order sweep-and-IOC-drop, O(1) cancel, stale-handle rejection, amend semantics, pool exhaustion. |

## Core data structures

### `Order` — one cache line, exactly

```cpp
struct alignas(64) Order {
    OrderId       order_id;   // generation<<32 | pool slot
    Price         price;
    Quantity      quantity;   // remaining, not original
    uint32_t      prev, next; // intrusive doubly-linked list (pool indices, not pointers)
    Side          side;
    bool          active;
    std::byte     _pad[...];  // explicit padding out to 64 bytes
};
static_assert(sizeof(Order) == 64);
```

`prev`/`next` are **indices into the pool array**, not pointers. That keeps
every order 4 bytes smaller than a pointer-based node on 64-bit systems,
keeps the whole pool relocatable in principle, and — most importantly — is
what lets `MemoryPool` be a single contiguous, cache-friendly array instead
of a graph of individually-`new`'d nodes scattered across the heap.

### `MemoryPool<T, Capacity>`

A `std::array<T, Capacity>` plus a `std::array<uint32_t, Capacity>`
free-list stack. `acquire()` pops an index, `release()` pushes it back. Both
are O(1), branchless in the common case, and touch no allocator. The pool is
sized and constructed exactly once at startup — "zero heap allocation" means
zero allocation *while processing orders*, which is what the constraint is
actually protecting against (allocator lock contention / page faults / unpredictable
latency spikes mid-match), not "the program never calls `new`."

### `SummaryBitmap<NumBits>` — O(1) best price

The hard part of "flat array instead of `std::map`" is: if you have 4,096
price levels and only 3 are occupied, how do you find the best one without
scanning up to 4,096 slots? A two-level bitmap:

- **Level 0**: one bit per price level, packed into 64-bit words (64 words
  for 4,096 levels).
- **Level 1**: a single `uint64_t` "summary" word — bit *i* is set iff
  level-0 word *i* is non-zero.

`find_lowest()` / `find_highest()` are then: one load + `countr_zero`/`countl_zero`
on the summary word to find *which* 64-level block has anything in it, then
the same op on that one level-0 word to find the exact level. Two words
touched, two hardware bit-scan instructions, regardless of how sparse the
book is. This is the same class of trick used by real venues and is why the
project caps at 4,096 price levels by default — the level-1 summary must fit
in one machine word for the "true O(1)" property to hold without a third
level. (Scaling past that is a documented one-line extension: add a
level-2 summary over the level-1 words.)

### `SPSCQueue<T, Capacity>`

Classic Vyukov-style single-producer/single-consumer ring buffer:

- `head_` (consumer-owned) and `tail_` (producer-owned) are each pinned to
  their own cache line via `alignas(hardware_destructive_interference_size)`,
  so the producer spinning on `tail_` and the consumer spinning on `head_`
  never invalidate each other's cache line (false sharing is the usual way
  "lock-free" code ends up slower than a mutex in practice).
- `push()` does a `relaxed` load of its own `tail_`, an `acquire` load of
  the *other* thread's `head_` (to safely check "is there room"), a plain
  write, then a `release` store to publish `tail_`. `pop()` is the mirror
  image. No `seq_cst`, no CAS loop — SPSC doesn't need either.

## Complexity

| Operation | Complexity | Notes |
|---|---|---|
| `match_or_add` (no cross) | O(1) | one pool `acquire`, one intrusive list append |
| `match_or_add` (crosses book) | O(k) | k = number of resting orders actually consumed — intrinsic to the operation, not overhead |
| `cancel_order` | O(1) | handle decode + validate, doubly-linked-list unlink, pool release |
| `amend_order` (qty decrease, same price) | O(1) | in-place mutation, keeps queue position |
| `amend_order` (price change / qty increase) | O(1) amortized | cancel + re-insert (cancel-replace, loses priority — matches real exchange behavior) |
| best bid / best ask | O(1) | two-level summary bitmap, ≤2 bit-scan instructions |

## Build & run

```bash
g++ -O3 -std=c++20 -pthread -Iinclude src/main.cpp -o orderbook_bench
./orderbook_bench

g++ -O2 -std=c++20 -pthread -Iinclude tests/test_orderbook.cpp -o orderbook_tests
./orderbook_tests
```

Or with CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/orderbook_tests
./build/orderbook_bench
```

## Benchmark harness

`main.cpp` spins up two threads:

- **Producer**: generates 100,000 randomized crossing limit/market orders
  (prices clustered so buys and sells actually cross) and pushes them into
  the SPSC ring buffer.
- **Consumer**: drains the ring buffer, calls `match_or_add()` on the
  engine, and records the `steady_clock` latency of that single call. Latency
  samples are pre-reserved (`reserve(100'000)`) before the timed loop starts
  so the measurement itself doesn't allocate.

Output includes throughput (orders/sec) and mean/p50/p90/p99/p99.9/max
latency in nanoseconds — the numbers you'd actually be asked to produce and
explain in an HFT systems interview.

## Known scope boundaries (and why)

Being upfront about these is more credible than pretending they don't
exist:

- **4,096 price-tick range by default.** A direct consequence of the
  two-level bitmap fitting in one summary word. Documented extension path:
  a 3-level bitmap for wider ranges (same technique, one more level).
- **Single matching-engine thread.** Sharding by symbol across multiple
  engine instances (each single-threaded, each fed by its own SPSC queue)
  is the standard way real systems parallelize this — a shared mutable book
  matched by multiple threads is a correctness hazard, not a performance
  win.
- **No persistence / recovery log.** Out of scope for a benchmark harness;
  a real venue would journal every accepted order for replay/audit.
- **IOC-only market orders.** Market orders sweep the book and drop any
  unfilled remainder rather than resting — this is standard market-order
  semantics, not a shortcut.
