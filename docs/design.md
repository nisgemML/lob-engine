# Design Notes: Low-Latency Trading Engine

These notes document the reasoning behind every non-obvious decision in the
codebase. They're written for an audience that will ask "why not X?" and
expects a real answer.

---

## 1. Why single-threaded matching?

The first instinct for a high-throughput system is to parallelize. For a limit
order book this is almost always wrong, and it's important to understand why.

A limit order book has a fundamental **sequential constraint**: the result of
matching order N can determine whether order N+1 matches at all (e.g. order N
exhausts a level, changing the best ask). There is no safe way to pipeline
across this boundary without coordination.

The options for parallelism are:

**a) Per-symbol sharding:** Each symbol runs on its own core with its own book.
Orders for different symbols are truly independent and this parallelizes cleanly.
This is what production systems do, and the `MatchingEngine::register_symbol`
API is designed to support it — each `OrderBook` is an independent object with
no shared state. Adding per-symbol threads is a mechanical change.

**b) Speculative execution:** Process orders speculatively in parallel, then
detect and replay conflicts. This adds complexity proportional to conflict rate
and is only profitable if conflicts are rare (< ~5%). For a continuous two-sided
book with crossing orders this assumption does not hold.

**c) Lock-based parallelism:** Adding a mutex to the matching path is strictly
worse than single-threaded: you pay the synchronization cost on every order even
when there's no actual contention. A lock acquisition on an uncontended mutex
takes ~15–30 ns on modern hardware — that's our entire p50 budget.

The baseline single-threaded design is correct, fast, and the right foundation
before adding sharding.

---

## 2. Memory ordering in the SPSC queue

The SPSC queue uses exactly two atomic variables and the weakest memory
orderings that still guarantee correctness. This is worth spelling out precisely
because it's easy to get wrong.

```
Producer                          Consumer
────────────────────────────────  ────────────────────────────────
buffer_[wp] = item;               if (rp == write_pos_.load(acquire))
write_pos_.store(next, release);      return false;    // empty
                                  out = buffer_[rp];
                                  read_pos_.store(next, release);
```

**Why `release` on the producer's write_pos store?**

The `release` establishes a happens-before relationship: any write that
precedes the `release` store (the payload write `buffer_[wp] = item`) is
guaranteed to be visible to any thread that subsequently does an `acquire`
load of the same variable.

Without `release`, the CPU (or compiler) is free to reorder the index update
before the payload write. The consumer would see the updated index, load an
uninitialized slot, and produce garbage.

**Why `acquire` on the consumer's write_pos load?**

The `acquire` is the receiving end of the release/acquire pair. It ensures the
consumer sees all writes that happened before the producer's `release` store.
Without it the consumer might load the old (pre-write) contents of `buffer_[rp]`
even after observing the updated `write_pos`.

**Why `relaxed` for the producer's read of `read_pos`?**

The producer reads `read_pos` only to check for fullness. It doesn't need to
synchronize any payload data — it just needs a recent-enough value to avoid
overwriting live slots. A `relaxed` load is sufficient: if the consumer has
advanced `read_pos` but the producer hasn't seen it yet, the producer will
think the queue is full and spin, then retry. This is safe (just a brief
stall) and avoids an unnecessary barrier on the common path.

**The total barrier count on the hot path:**
- Producer fast path (not full): 1 relaxed load + 1 payload write + 1 release store
- Consumer fast path (not empty): 1 relaxed load + 1 acquire load + 1 payload read + 1 release store

No explicit fence instructions. The compiler generates `MFENCE`/`SFENCE` only
where the release/acquire semantics require it (which on x86-TSO is typically
a `MOV` with implicit store-store ordering for releases, and a `MOV` with
acquire semantics for loads — essentially free on TSO).

---

## 3. Struct-of-Arrays order book layout

### The pointer-based baseline

A textbook LOB uses:
```cpp
std::map<Price, std::list<Order*>> bids;
```

Walking to the best bid is O(1) amortized (the map keeps sorted order). But
matching requires:
1. Tree node lookup → random pointer chase → probable L2/L3 miss
2. List node traversal → random pointer chase → another probable miss
3. Order object access → another random pointer chase → another miss

At 100 ns/cache miss × 3 misses per match = ~300 ns just in cache misses,
before any actual work is done.

### The SoA transformation

```cpp
struct Side {
    Price    prices[kMaxLevels];       // hot: iterated during matching
    Qty      qtys[kMaxLevels];         // cold: touched only on match
    uint32_t order_counts[kMaxLevels]; // cold
    uint32_t head_idxs[kMaxLevels];    // head of per-level order chain
    uint32_t tail_idxs[kMaxLevels];    // tail of per-level order chain (O(1) append)
};
```

The matching loop iterates `prices[]` to find the crossing level:
```cpp
while (incoming.qty_remaining > 0 && passive_side.count > 0) {
    Price best_passive = passive_side.prices[0]; // always index 0 (best)
    if (incoming.price < best_passive) break;
    ...
}
```

Since we always work the front of the sorted array, `prices[0]` is almost
always in L1. For deeper books (scanning multiple levels), the sequential
access pattern lets the hardware prefetcher pull in the next cache line before
we need it.

`qtys[]` and `order_counts[]` are only accessed after a cross is confirmed.
By keeping them in separate arrays, the price comparison loop doesn't pollute
the cache with quantity data that's not needed for the decision.

### The level-index stability problem

The design went through one significant bug: the original implementation stored
a `level_idx` (array index) in each order slot to make cancel O(1). But
`remove_level()` uses `memmove` to shift the sorted arrays — invalidating any
stored indices.

The fix stores the order's **price** instead. Cancelling an order requires
a `find_level(price)` call — O(depth) — but depth is typically small (< 20
levels with active resting orders) and the scan is cache-friendly. This is the
right trade-off: O(1) cancel via stale index is only O(1) if the index stays
valid, which it doesn't after any level removal.

An alternative would be to lazily update stored indices after every `memmove`.
But updating all live orders' stored indices after a level removal requires
scanning all slots — worse than the linear scan approach.

### The O(n) tail-walk problem

The original append-to-level implementation walked the intrusive linked list
to find the tail on every passive order insertion:

```cpp
// O(n) — walks every order at the level on each insert.
uint32_t cur = side.head_idxs[lvl_idx];
while (slots_.nexts[cur] != NULL_IDX) cur = slots_.nexts[cur];
slots_.nexts[cur] = slot;
```

For a busy options strike with many resting orders (common at the at-the-money
level), this degrades passive insertion from O(1) to O(orders-at-level). At
1000 resting orders per level, an incoming limit order that doesn't cross would
spend most of its "passive insertion" time on this walk.

The fix adds `tail_idxs[]` to the `Side` struct — one `uint32_t` per level,
in the same SoA layout as `head_idxs[]`. Append becomes two pointer writes:

```cpp
// O(1) — direct tail pointer update.
slots_.nexts[side.tail_idxs[lvl_idx]] = slot;
side.tail_idxs[lvl_idx]               = slot;
```

`tail_idxs[]` is maintained in `insert_level` (initialised to `NULL_IDX`),
`remove_level` (shifted alongside `head_idxs[]` in the `memmove`),
`try_match` (reset when the head slot empties the level), and
`remove_order_from_level` (updated when the tail slot is cancelled).

---

## 4. The pool allocator

### Why not `malloc`?

`malloc` in glibc uses a thread-local arena + a global fallback. Even the
happy path (local arena, no contention) involves:
- Checking the freelist for the right size class
- Updating the freelist pointers
- Potentially zeroing memory
- A store fence to ensure visibility

This is ~50–100 ns for a small allocation and non-deterministic in the worst
case (fragmented heap, arena lock contention).

### The slab design

```
[mmap'd page] [Order][Order][Order]...[Order]
               ↑                            ↑
               slab_                        slab_ + N*sizeof(Order)
```

The free list is threaded through the slab itself — each free slot's first
bytes hold a pointer to the next free slot. No external metadata.

`mlock` pins the pages. After startup, every allocation and deallocation is:
1. Load `free_head`
2. Follow one pointer
3. Store new `free_head`

Three memory accesses, no system calls, no locks. If `free_head` is in L1
(likely — it's touched constantly), allocation is ~3–5 ns.

### Why not per-thread pools?

The matching engine is deliberately single-threaded, so there's no contention
and no need for per-thread pools. Adding per-thread pools would be the right
move if we sharded matching across cores (one pool per matching thread, no
synchronization needed).

---

## 5. CPU isolation and scheduling

The matching engine thread does two things to minimize latency jitter:

**a) Core affinity (`pthread_setaffinity_np`):**

Pinning to a specific core means the OS scheduler will not migrate the thread
to another core mid-execution. Core migration causes TLB flushes and L1/L2
cache invalidation — potentially hundreds of nanoseconds of disruption.

In production, the target core is also removed from the Linux scheduler's
general pool (`isolcpus=2` kernel parameter) and from interrupt routing
(`/proc/irq/*/smp_affinity`), so no other work ever runs on it.

**b) SCHED_FIFO at priority 50:**

The default scheduler (CFS) will preempt the matching thread when its
timeslice expires (typically 1–4 ms). A preemption stall is catastrophic for
p99.9 latency.

`SCHED_FIFO` is a realtime policy: the thread runs until it voluntarily yields
or blocks. Since the matching loop never blocks (it busy-polls the SPSC queue),
it will not be preempted.

**c) PAUSE instruction:**

When the inbound queue is empty, the loop spins with `__builtin_ia32_pause()`
(compiles to the x86 `PAUSE` instruction). This does two things:
- Signals to the CPU's memory order speculation that we're in a spin-wait,
  reducing speculative load penalties
- Reduces power consumption slightly (prevents the CPU from burning all
  its power budget spinning, which could cause thermal throttling)

**d) A real hazard this design missed, found while extending it: SCHED_FIFO
without a dedicated core is actively dangerous, not just less beneficial.**

`MatchingEngine::start()` used to apply `SCHED_FIFO` unconditionally,
regardless of whether the pin to a specific core actually succeeded. That's
fine on the intended deployment (a genuinely isolated core) — but this
codebase's own `MultiSymbolEngine` starts N shards, each calling
`start()`, and on any machine with fewer cores than shards (routine in CI,
in a shared dev box, or in this project's own container-based benchmark
runs), several of those threads end up SCHED_FIFO on the SAME core.

Measured directly, not assumed: two busy-poll `SCHED_FIFO` threads at the
same priority sharing one CPU split scheduled iterations roughly **8,800:1**
in a 300ms window — severe, though the Linux RT-throttling safety valve
(reserving a small default slice for non-realtime work) keeps it short of
a full deadlock. This is exactly the failure mode you'd expect from a
scheduling policy whose entire contract is "keep running until you block
or a higher-priority thread preempts you," applied to more busy-poll
threads than there are cores to run them on.

**First fix (superseded):** `start(int cpu_id)` initially coupled the two
by *request* — `cpu_id < 0` skipped pinning **and** `SCHED_FIFO` together.
That closed the case above, but left a different, related gap: a caller
that *did* pass `cpu_id >= 0`, on a machine where that specific core
doesn't exist, would have its pin attempt fail (`pthread_setaffinity_np`
returning `EINVAL`) while `SCHED_FIFO` still got applied anyway — because
the gate checked what was *asked for*, not what actually happened.
Confirmed directly, not hypothetically: `bench_replay` on a 1-core
sandbox, requesting `cpu_id=1` (a core that doesn't exist there), printed
`pinned=no realtime=yes` — the exact hazard this section exists to guard
against, reached through a path the first fix didn't cover.

**Current fix:** `SCHED_FIFO` is now gated on whether the pin *actually
succeeded* (`is_pinned()` after the `pthread_setaffinity_np` call), not on
what `cpu_id` was requested. A caller whose pin attempt failed — whether
because it explicitly passed `cpu_id = -1`, or because it asked for a real
core number that turned out not to exist on this machine — has no
dedicated core either way, and gets no `SCHED_FIFO` elevation either way.
Verified this closes the gap: the same `bench_replay` run above now
correctly reports `pinned=no realtime=no`. See `matching_engine.cpp`'s
`start()` for the full reasoning, and `bench/bench_multisymbol.cpp` for
where the original hazard was found (every shard there ran unpinned by
default on this project's own single-core sandbox, with a `--pinned` flag
added once real multi-core hardware was available to test the intended
case for real).

---

## 6. The order index hash map

Cancel requires O(1) lookup from `OrderId` to `(slot, side)`. The standard
choice is `std::unordered_map`, but it has several problems on the hot path:

- Heap allocation for bucket arrays and chained nodes
- `std::hash<uint64_t>` is typically modulo-based — poor distribution
- Iterator invalidation on rehash causes unpredictable latency spikes

The implementation uses a hand-rolled open-addressed hash table with:

**Fibonacci hashing:** `id * 2654435761 >> 32`

Fibonacci hashing (multiplication by the golden ratio, scaled to the hash
table size) distributes keys more uniformly than modulo-based hashing because
it exploits the full bit width rather than just the low-order bits. For
sequential `OrderId` values (which are common), modulo hashing would cluster
all entries into the same few buckets.

**Fixed-size table (load factor ~0.5):**

No rehashing, ever. The table is allocated upfront at 2× the maximum order
count. At 0.5 load factor, expected probe length is ~1.5 slots on lookup.

**Backward-shift deletion:**

On deletion, we use backward-shift to fill the hole rather than marking the
slot as a tombstone. Tombstones accumulate over time and degrade lookup
performance as the probe chains lengthen. Backward-shift deletion maintains
the invariant that every key is as close as possible to its natural hash slot
by re-hashing and repositioning any displaced entries that were only at their
current slot because of the deleted entry's former occupancy.

Note: this is **not** Robin Hood hashing. Robin Hood is an *insertion* strategy
that swaps a new key with an incumbent if the new key has probed farther from
its natural slot than the incumbent has — minimising variance in probe lengths.
The current implementation uses standard linear probing with backward-shift
deletion, which is correct and sufficient given the bounded load factor (~0.5).
Robin Hood insertion would reduce mean probe length further but adds complexity
to the insert path.

---

## 7. What the latency numbers actually measure

The benchmark measures wall-clock time from SPSC push to SPSC pop including
all of:
- Producer-side: SPSC write barrier + index store
- Inter-thread communication: cache coherence traffic (MESI state transitions)
- Consumer-side: SPSC acquire load + message decode
- Order book: price comparison, level insertion/lookup, slot allocation, chain append
- Execution report: SPSC push on the outbound queue
- Back to producer: SPSC acquire load on the outbound queue

**What it does NOT include:**
- Network I/O (kernel bypass / DPDK)
- Clock synchronization (PTP/GPS for timestamping)
- NIC timestamping

In a production co-located system, the dominant latency is typically the NIC
receive path (DPDK + busy-poll kernel bypass gets to ~1 µs from wire to
application), and then the matching engine adds the ~400–500 ns we measure here.
The round-trip from a market data update to an outbound order is therefore
~2–3 µs — competitive with co-located HFT systems.

---

## 8. Things deliberately left out

**Persistence / crash recovery:** this section used to say this was left
out entirely — it wasn't quite true even before this update (the pieces
already existed, just framed for a different purpose) and is actively
misleading now. `TraceWriter`/`OrderFlowReplay` (`include/core/replay.hpp`)
already record every order event with a timestamp and can replay them
through a fresh `MatchingEngine` — built and used for latency
benchmarking (`bench/bench_replay.cpp`), and proven byte-exact
deterministic by `tools/replay_trace.cpp`'s own determinism check.
`examples/recovery_demo.cpp` demonstrates that this is, structurally,
exactly what a write-ahead log needs: log every event as it happens,
destroy the engine, reconstruct a fresh one, replay the log, and the
resulting book state matches pre-crash state exactly — verified by
content (best bid/ask), not just "didn't crash." What's genuinely still
absent: durability guarantees for the trace file itself (no fsync
policy, no answer for a crash mid-write to the log), log rotation, and
compaction — real engineering problems a production WAL has to solve
that this demo doesn't attempt to.

**Network transport:** The `MarketDataIngestion` interface accepts a raw
`span<uint8_t>`. Plugging in DPDK, RDMA, or kernel UDP is a one-function
change. Left out because transport is orthogonal to matching correctness and
latency. `examples/feed_to_execution_demo.cpp` demonstrates the missing
piece this used to leave implicit: something has to drain
`MarketDataIngestion`'s output queue and call `MatchingEngine::submit()`
for each message — the two components had no code anywhere connecting
them before that file existed, only separate test coverage for each half.

**Risk / pre-trade checks:** Fat-finger limits, position limits, credit checks.
These live between ingestion and matching. They are latency-sensitive but not
architecturally interesting — just comparisons against pre-computed limits.

**Order types:** IOC and FOK are partially implemented. GTC, GTD, pegged
orders, iceberg orders, and stop-loss orders each require additional state and
matching logic. The framework supports adding them without architectural change.

**Cross-symbol arbitrage detection:** Detecting spread relationships across
symbols (e.g. cash-futures basis) requires a global view across books. This
is the job of the strategy layer, not the matching engine.

---

## 9. A hazard class this design creates: large fixed-size objects as stack locals

This codebase's core design principle — no heap allocation on the hot
path, so `OrderBook`, `MatchingEngine`, and `MarketDataIngestion::
OutboundQueue` all hold their working state in fixed-size arrays directly
as members — has a real, repeatedly-encountered downside: these objects
are large (`sizeof(OrderBook)` ≈ 4.4MB, `sizeof(MatchingEngine)` ≈ 5.5MB,
`sizeof(MarketDataIngestion::OutboundQueue)` ≈ 2.5MB), and the default
Linux stack limit is 8MB. Declaring more than one of these as a plain
local variable in one function — not behind a pointer — can silently
approach or exceed that limit.

This is not a hypothetical concern raised in the abstract: it was hit
**three separate times** while extending this codebase, in three
different files:

1. `tests/test_order_book.cpp`'s `BookFixture` held `OrderBook` as a
   direct member; `test_fok_atomicity` constructs three such fixtures in
   one function. Confirmed with a real crash:
   `AddressSanitizer: stack-overflow ... in test_fok_atomicity` (ASan's
   stack-redzone overhead was what pushed an already-borderline case over
   the edge — the same test passed under a plain Release build).
2. `examples/feed_to_execution_demo.cpp`'s first version declared
   `MatchingEngine` and `MarketDataIngestion::OutboundQueue` as plain
   locals in `main()` — 5.5MB + 2.5MB = exactly 8.0MB before a single
   other local variable. It segfaulted immediately, unconditionally, on
   every run.
3. `examples/recovery_demo.cpp` and `tools/soak_test.cpp` avoided the
   same mistake from the start, once the pattern was recognized.

**The fix, applied everywhere this was found:** heap-allocate via
`std::make_unique<T>()` and hold a reference or pointer instead, the
moment more than one of these types might land in the same function's
stack frame. This is also `MatchingEngine`'s own established pattern in
production use — its `books_` member is
`std::array<std::unique_ptr<OrderBook>, kMaxSymbols>`, not an array of
`OrderBook` by value — so heap-allocating these types in test/example
code isn't a workaround, it's using the codebase the way its own
production code already does.

**If you're extending this codebase:** before declaring a local variable
of type `OrderBook`, `MatchingEngine`, `MultiSymbolEngine<N>`, or
`MarketDataIngestion::OutboundQueue` (or any type containing one as a
direct member), ask whether anything else in the same function might also
need one — if so, heap-allocate both.
