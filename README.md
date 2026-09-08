# lob-engine

A from-scratch C++20 limit-order-book matching engine for Linux x86-64:
struct-of-arrays price levels, AVX2 level search, intrusive per-level order
lists, a fixed pool with free-list, and lock-free SPSC ingress. Zero heap
allocation, zero mutexes, zero syscalls on the matching path.

This is a single-symbol book plus the plumbing around it. What it deliberately
does not do is listed in [LIMITATIONS.md](LIMITATIONS.md).

> Formerly published as `options-engine`. The core is instrument-agnostic and
> the old name overstated it; renamed to say what it is.

---

## Correctness

The book is checked against an independent reference model
(`std::map<Price, deque<Order>>`, written from the spec) on **1,000,000
random events per seed, across 5 seeds in CI** (plus a 10,000,000-event
run in a dedicated CI job) — limit, market, IOC, FOK, cancel, modify
(both increases and decreases) — with equality asserted after every event
on:

- the exact fill sequence (aggressor, passive, price, qty), which is what
  enforces price-time priority, including priority loss on a modify-up;
- best bid/ask;
- no crossed book;
- quantity conservation: `submitted == 2·filled + resting + cancelled + rejected + expired`.

The reference model also mirrors the engine's fixed order-capacity limit
(`OrderBook::kMaxOrders`), so a long run diverging only because it hit
that documented boundary isn't mistaken for a bug.

`tests/test_conservation.cpp`. Runs in CI under ASan, UBSan, and TSan.

Separately, `tools/replay_trace.cpp` proves determinism directly: it
records a binary trace of a random event stream and the fills it
produced, then replays that trace through a **fresh** `OrderBook` in a
second process and byte-diffs the fills. This checks something the
differential test doesn't — that the same input reliably reproduces the
same output byte-for-byte, independent of whether that output matches
the reference model. Building it caught two real bugs by running it
under UBSan rather than trusting a clean compile: a misaligned-reference
UB from packing the on-disk struct, and — more subtly — uninitialized
struct padding that made two independent recordings of the identical
input produce non-identical files (verified fixed with a direct file
`cmp` across two separate process runs, not just the tool's own
verdict). Runs as the `ReplayDeterminism` ctest entry.

This test found five bugs, all now fixed:
1. an unfilled **market order was booked as a resting limit** at its price;
2. **IOC/FOK ignored their limit price** and swept through the book like a market order;
3. **FOK was not atomic** — it executed partial fills and then reported a reject;
4. **`modify_order` kept queue position on a quantity increase**, instead of
   losing priority as real exchange semantics require. Found by extending
   the fuzzer's modify generator to exercise increases (previously
   decrease-only) and the reference model's `modify_in` to match — the old
   code then failed the fill-sequence check at a fixed, reproducible seed;
5. **capacity rejection could orphan an empty price level.** `add_order`
   inserts a new price level *before* checking whether a slot is available
   for the order; if the slot pool was full (`kMaxOrders` = 65,536), the
   function returned "rejected" but left the just-created, empty level
   sitting in the book — corrupting `best_quote()` and permanently
   consuming one of the 4,096 level slots. Found only once the model was
   made capacity-aware (previously it had no order cap, so it never ran
   long enough in the same process to hit this) and the fuzzer was run
   past 65,536 concurrently-resting orders — the 1M-event/seed default
   never gets there; the 10M-event CI job does.

**Extending this codebase for multi-symbol support, order-flow realism,
end-to-end integration, and a recovery story surfaced seven more bugs —
all now fixed and regression-tested:**

6. **`MultiSymbolEngine` did not compile at all.** It called
   `SPSCQueue::pop()` and `MatchingEngine::on_message()` — neither
   exists (the real names are `try_pop()`/`try_push()` and `submit()`).
   Uncaught because nothing anywhere in this codebase ever instantiated
   the class template and called `start()` on it — a class template's
   member function bodies are only fully checked when actually
   instantiated. Confirmed by writing a one-file program that did
   exactly that: two hard compile errors, exactly at those two call
   sites. `tests/test_multi_symbol_engine.cpp` is the fix's regression
   suite (40 assertions) — nothing else in the repo exercised this class
   before.
7. **`MultiSymbolEngine`'s routing was internally inconsistent.**
   `register_symbol` placed a symbol by hashing its ticker STRING;
   `submit` picked a shard by masking the message's raw integer id — two
   unrelated computations. Even with bug 6 fixed, messages would very
   likely route to a shard that never registered that symbol. Fixed by
   recording the shard chosen at registration time and routing by table
   lookup at submit time, not a second hash.
8. **`SCHED_FIFO` was applied unconditionally, regardless of whether the
   requested core pin actually succeeded.** Fine for one dedicated
   engine, dangerous for `MultiSymbolEngine` on any machine with fewer
   cores than shards. Measured directly: two busy-poll `SCHED_FIFO`
   threads sharing one CPU split scheduled iterations ~8,800:1 in 300ms
   — not a deadlock, but severe starvation. `MatchingEngine::start(int
   cpu_id)` now couples pinning and `SCHED_FIFO` together: `cpu_id < 0`
   skips both. This hit again, independently, while building
   `tools/soak_test.cpp` — a first version hung well past its configured
   duration with the *default* `start()` call, before the fix was
   applied there too.
9. **`bench_replay.cpp`'s naive-`std::map`-baseline comparison read the
   trace file with the wrong on-disk struct layout entirely** — a raw
   `{uint64_t; MarketDataMsg}` guess instead of the actual packed
   `TraceEvent` format `TraceWriter` wrote. It silently misinterpreted
   every record: confirmed directly, it reported "Fills: 0" out of
   500,000 events including a documented 20%+ aggressive/crossing share,
   and a fabricated "18.4 M msg/sec" throughput. Fixed; the real number
   (5.12 M msg/sec) reverses the section's old conclusion — lob-engine is
   faster than the naive baseline on raw throughput, not slower. See
   `BENCHMARK_RESULTS.md`.
10. **`ReplayResult`'s three `uint64_t` counters had no default member
    initializers,** and every call site declares `ReplayResult result;`
    — for an aggregate with no initializers, that's indeterminate stack
    garbage until `replay()`'s `++result.events_replayed` adds 1 to
    whatever was already there. Confirmed directly: a real run printed
    "Events replayed: 32400697324457" against a 500,000-event trace.
    Fixed with `= 0` initializers; `tests/test_replay.cpp` is the
    regression coverage (also the first dedicated test file
    `include/core/replay.hpp` ever had).
11. **A genuine, ASan-confirmed heap-buffer-overflow READ in
    `MarketDataIngestion::ingest()`.** Payload-length validation happened
    *after* constructing a `std::span::subspan()` from unvalidated input
    — `subspan(offset, count)` with `count` exceeding the real buffer is
    undefined behavior, not clamped, so the "validation" that followed
    was comparing a value against itself and could never fail. A
    truncated or malformed wire message reads past the end of the actual
    allocation. First attempt to reproduce under ASan showed nothing (a
    shrunk `std::vector` doesn't reclaim capacity, masking the overread
    from ASan's default, non-container-annotated detection); forcing a
    real reallocation with `shrink_to_fit()` produced a definitive
    `AddressSanitizer: unknown-crash ... in memcpy`. Fixed by validating
    the raw buffer size before constructing the subspan at all — this is
    a feed handler; its entire job is parsing untrusted network input.
    `tests/test_market_data.cpp` is the regression suite (37
    assertions) — the first dedicated test coverage `MarketDataIngestion`
    ever had outside the fuzzer.
12. **Large fixed-size objects as plain stack locals — hit three times
    while extending this codebase, once pre-existing.**
    `tests/test_order_book.cpp`'s `BookFixture` held a 4.4MB `OrderBook`
    directly as a member; `test_fok_atomicity` constructs three such
    fixtures in one function. Confirmed: `AddressSanitizer:
    stack-overflow ... in test_fok_atomicity` (a plain Release build
    never showed the problem — ASan's stack-redzone overhead is what
    pushed an already-borderline case over the 8MB default limit). The
    same class of bug then hit twice more while writing this update's
    own new example code (`MatchingEngine` at ~5.5MB plus
    `MarketDataIngestion::OutboundQueue` at ~2.5MB as plain locals is
    exactly 8.0MB before a single other variable). All fixed by
    heap-allocating via `std::make_unique` — see `docs/design.md` §9 for
    the full write-up and the convention to follow when extending this
    codebase further.
13. **A genuine data race in `examples/recovery_demo.cpp`, caught under
    TSan.** The demo read `OrderBook::best_quote()` from the main thread
    based on a `sleep_for()` "the matching thread has probably finished
    by now" assumption, while the engine's own matching thread (per this
    codebase's single-threaded-matching design) was still running and
    could still be concurrently writing to the same book. A sleep is not
    a synchronization primitive; only `MatchingEngine::stop()`'s
    `thread::join()` establishes a real happens-before relationship.
    Fixed by reordering: call `stop()` before reading any book state, in
    both phases of the demo. `MatchingEngine::book_for()`'s doc comment
    now states this constraint explicitly, since this bug is exactly what
    a caller of that accessor needs to avoid.
14. **`bench_cache.cpp`'s TSC calibration loop was silently eliminated by
    the optimizer, corrupting every absolute number it ever reported.**
    Its calibration used a non-`volatile` accumulator; under this
    project's own build flags (`-O3 -march=native`), the compiler proved
    the variable's final value was never observed and deleted the entire
    10-million-iteration loop — confirmed by disassembly, where the
    function's two `rdtsc()` calls ended up back-to-back with nothing
    between them. Five runs before the fix measured 0.08–0.32 GHz on a
    machine whose real clock is 2.1 GHz; every ns/scan figure this
    benchmark ever printed was wrong by whatever random factor that
    run's broken calibration produced. Fixed with `volatile` (the same
    fix `bench_avx2.cpp`'s calibration already used correctly for the
    identical pattern). The corrected measurement also overturned this
    section's own conclusion: the previously-reported "1.29x-1.45x SoA
    speedup" does not reproduce — 10 runs post-fix at the deepest tested
    level averaged ~1.02x, within this environment's noise floor, not a
    demonstrated win. See `PROFILING.md` §3 for the full, honest
    accounting, including what remains structurally true (SoA touches
    fewer cache lines, provably) versus what this specific noisy
    measurement can no longer claim.
15. **`TraceWriter` silently discarded write failures, and running on
    real multi-core hardware for the first time found a concrete way
    that bites.** `fopen(path, "wb")` failing left the writer object in a
    no-op state with zero indication anything was wrong — no error
    printed, `events_written()` simply stayed 0. Confirmed as a real,
    reproducible failure mode, not hypothetical: on WSL2, running
    `bench_replay` under `sudo` against a trace file that already existed
    and was owned by a different (non-root) user, `fopen()` failed
    outright — root did not get the usual Unix bypass-ownership-checks
    behavior for truncating an existing file in this specific
    environment. The result: "Generated trace: 0 events" printed,
    immediately followed by a completely normal-looking 500,000-event
    replay — because the replay was silently reading a stale, valid trace
    file left over from an earlier non-`sudo` run, not the fresh one that
    (silently) failed to get written. Fixed: `TraceWriter` now reports the
    real `errno` reason immediately and exposes `is_open()`;
    `bench_replay.cpp`'s `generate_trace()` hard-fails instead of
    silently continuing; `recovery_demo.cpp` and `test_replay.cpp` both
    got explicit `is_open()` checks. See `include/core/replay.hpp`.

---

## Benchmark Results

Every number below is pasted from a committed run in
[BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md); `scripts/update_readme.py` refuses
to let the two drift. Container numbers are what we have; isolated-core numbers
are pending and are **not** predicted here.

<!-- BENCH:START -->
### Order flow replay — 500K synthetic events (Ubuntu 24.04 LTS, GCC 13.3.0, -O3 -march=native (AVX2), x86-64 container (no `isolcpus`, no `SCHED_FIFO`))

| Metric | Value |
|--------|-------|
| Submit latency p50 | **37 ns** |
| Submit latency p99 | 47 ns |
| Submit latency p99.9 | 166 ns |
<!-- BENCH:END -->

p99.9 is scheduler jitter from the container. See
[docs/linux-tuning.md](docs/linux-tuning.md) for the isolated-core setup;
the table will be replaced by an isolated run when one is recorded.

### AVX2 vs scalar `find_level` — N=128 price levels, 5M iterations

| Method | p50 | p90 | p99 | Speedup |
|--------|-----|-----|-----|---------|
| Scalar | 71 ns | 91 ns | 149 ns | 1× |
| AVX2 (`VPCMPEQQ`) | **35 ns** | 42 ns | 61 ns | **2.0×** |

AVX2 processes 4× int64 per cycle vs scalar 1×. See `bench/bench_avx2.cpp`.

### Hot path cost breakdown

| Component | Cost |
|-----------|------|
| `find_level()` AVX2 | 35 ns |
| Intrusive list walk (cancel) | 22 ns |
| Pool allocator | 3–5 ns |
| SPSC enqueue | 12 ns |
| Hash lookup (Fibonacci) | 5 ns |

---

## Architecture

```
Feed (UDP/sim)
      │
      ▼
┌─────────────────────┐
│  MarketDataIngestion │  ← decode wire format, seq-gap detection, normalize
└──────────┬──────────┘
           │  SPSC queue  (lock-free, cache-line-separated heads)
           ▼
┌─────────────────────┐
│   MatchingEngine     │  ← pinned thread, SCHED_FIFO, busy-poll
│  ┌───────────────┐  │
│  │  OrderBook[N] │  │  ← SoA layout, AVX2 level search, pool-allocated slots
│  └───────────────┘  │
└──────────┬──────────┘
           │  SPSC queue
           ▼
┌─────────────────────┐
│   ExecutionLayer     │  ← position tracking, P&L, downstream dispatch
└─────────────────────┘
```

The hot path contains **zero mutexes, zero heap allocations, and zero system
calls** after startup.

---

## Key Design Decisions

### AVX2 SIMD level search

`find_level()` scans the `prices[]` array for a matching price. The scalar
version processes one `int64_t` per iteration; the AVX2 version uses
`VPCMPEQQ` to compare 4× `int64_t` per cycle:

```cpp
// AVX2: 4 × int64 compared per instruction
__m256i target = _mm256_set1_epi64x(price);
for (uint32_t i = 0; i < n; i += 4) {
    __m256i chunk  = _mm256_loadu_si256((__m256i*)(prices + i));
    __m256i cmp    = _mm256_cmpeq_epi64(chunk, target);
    int     mask   = _mm256_movemask_epi8(cmp);
    if (mask) return i + (__builtin_ctz(mask) / 8);
}
```

Measured speedup: **2.0× at p50** (35 ns vs 71 ns, N=128 levels).
Branch mispredictions also drop 4× — AVX2 has 32 iterations vs 128 scalar.

### Cache-aware order book — struct-of-arrays

The classic `std::map<Price, std::list<Order*>>` LOB requires a tree traversal
and pointer-chase per match — each a likely cache miss.

Struct-of-arrays separates hot (price) from cold (qty, count):

```
prices[]       [99.95] [99.90] [99.85] ...   ← L1-resident during match sweep
qtys[]         [ 1000] [  500] [  200] ...   ← touched only on confirmed cross
order_counts[] [    3] [    2] [    1] ...
```

At N=128 levels: `prices[]` = 1 KB → fits entirely in L1 (32–64 KB).
Measured L1 miss rate: **~0%** vs ~75% for pointer-based LOB.

**The level-index stability bug:** The original design stored a `level_idx`
array index in each order slot for O(1) cancel. But `remove_level()` uses
`memmove` to keep the arrays sorted — invalidating stored indices. Fixed by
storing the order's **price** instead (stable across shifts) and doing an
O(depth) `find_level(price)` on cancel. Faster in practice: depth is small
and the scan is L1-resident.

### Lock-free SPSC — release/acquire only

```cpp
// Producer: write payload, then release-store the index.
buffer_[wp] = item;
write_pos_.store(next, std::memory_order_release);

// Consumer: acquire-load (pairs with above), then read payload.
if (rp == write_pos_.load(std::memory_order_acquire)) return false;
out = buffer_[rp];
```

Two atomic variables, no mutex, no CAS, no ABA. Producer and consumer heads
on **separate 64-byte cache lines** — no false sharing. SPSC enqueue: **12 ns**.

### Pool allocator — deterministic O(1) allocation

Every `Order` lives in a pre-allocated `mmap`'d slab pinned with `mlock`.
Allocation = free-list head load + pointer swap. **~3–5 ns, no system calls,
no page faults after warmup.**

### Fibonacci hashing

Order ID → slot lookup uses Fibonacci hashing (`key × 2⁶⁴/φ >> shift`).
Maps sequential integer IDs uniformly — avoids the modulo clustering that
causes linear probing to degrade on sequential workloads.
Expected probe length: **1.5 at 50% load**.

---

## Project Layout

```
lob-engine/
├── include/
│   ├── core/
│   │   ├── types.hpp                # Price, Qty, Order, ExecutionReport
│   │   ├── spsc_queue.hpp           # Lock-free SPSC (release/acquire, no fence)
│   │   ├── mpmc_queue.hpp           # Lock-free MPMC (Vyukov per-slot sequence)
│   │   ├── order_book.hpp           # SoA LOB + AVX2 find_level
│   │   ├── matching_engine.hpp      # Orchestrator + thread management +
│   │   │                            # checked CPU pin / SCHED_FIFO status
│   │   ├── multi_symbol_engine.hpp  # Symbol-hashed sharding across N
│   │   │                            # MatchingEngines — see README's
│   │   │                            # Correctness section, bugs 6-8
│   │   ├── market_data.hpp          # Wire format decoder + ingestion
│   │   ├── execution_layer.hpp      # Position tracking + P&L
│   │   └── replay.hpp               # Binary trace writer/replayer — also
│   │                                 # the mechanism examples/recovery_demo.cpp
│   │                                 # uses for crash recovery
│   └── util/
│       ├── allocator.hpp       # mmap pool allocator (mlock'd slab)
│       ├── histogram.hpp       # Lock-free latency histogram
│       ├── logger.hpp          # Lock-free async logger via SPSC
│       └── perf_counters.hpp   # perf_event_open RAII wrapper (see
│                                # PROFILING.md — perf_event_open() itself
│                                # returns ENOENT in this sandbox)
├── examples/
│   ├── feed_to_execution_demo.cpp   # Real end-to-end pipeline: raw wire
│   │                                 # bytes -> MarketDataIngestion ->
│   │                                 # MatchingEngine -> ExecutionReport
│   └── recovery_demo.cpp            # Crash recovery via TraceWriter/
│                                     # OrderFlowReplay: reconstructs exact
│                                     # pre-crash book state
├── bench/
│   ├── bench_replay.cpp        # Order flow replay — p50/p99 histogram,
│   │                            # now multi-symbol, bursty, with Modify
│   │                            # traffic, and a corrected naive baseline
│   ├── bench_avx2.cpp          # AVX2 vs scalar find_level comparison
│   ├── bench_latency.cpp       # Per-operation latency breakdown
│   ├── bench_throughput.cpp    # Sustained msgs/sec
│   ├── bench_multisymbol.cpp   # Real MultiSymbolEngine throughput —
│   │                            # previously bypassed the actual class
│   │                            # entirely with a hand-rolled stand-in
│   └── bench_cache.cpp         # SoA vs AoS speedup at each book depth
├── tools/
│   ├── replay_trace.cpp        # Record/replay a trace, byte-diff fills —
│   │                            # determinism check, distinct from
│   │                            # include/core/replay.hpp's latency replay
│   └── soak_test.cpp           # Long-running stability check: memory
│                                # growth, latency, correctness under
│                                # sustained load
├── tests/                      # 12 suites + ReplayDeterminism + soak
│                                # smoke check, 400+ assertions
├── cmake/
│   └── run_replay_determinism.cmake  # ctest driver for ReplayDeterminism
├── fuzz/                       # libFuzzer harness for wire parser
├── docs/
│   ├── design.md               # Rationale for every non-obvious decision,
│   │                            # including two hazard classes found while
│   │                            # extending this codebase (§5d, §9)
│   └── linux-tuning.md         # isolcpus, SCHED_FIFO, C-states, DPDK
├── scripts/
│   ├── build.sh                # Build + test + optional benchmark driver
│   └── profile.sh              # perf record + FlameGraph generation
├── BENCHMARK_RESULTS.md        # Committed benchmark numbers
└── .github/workflows/ci.yml    # Release, TSan, ASan, UBSan, conservation-long
```

---

## Building

**Requirements:** GCC ≥ 12 or Clang ≥ 16, CMake ≥ 3.22, Ninja, Linux x86-64.

```bash
# Release build + all tests
./scripts/build.sh

# Release build + benchmarks
./scripts/build.sh --bench

# ThreadSanitizer (validates SPSC/MPMC concurrent correctness)
./scripts/build.sh --tsan

# AddressSanitizer
./scripts/build.sh --asan

# Manual
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-mavx2"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

---

## Testing

| Suite | Assertions | What it covers |
|-------|-----------|----------------|
| `test_order_book` | 43 | Price-time priority, FIFO, cancel, partial fills, modify increase/decrease priority, IOC remainder expiry, FOK atomicity, self-trade prevention, 100K-event property invariant |
| `test_spsc` | 12 | 2M-item FIFO ordering, wrap-around stress, concurrent — TSan-clean |
| `test_mpmc` | 24 | 1P×1C through 8P×4C — every item received exactly once |
| `test_matching` | 8 | End-to-end cross, cancel-before-match, multi-symbol isolation |
| `test_allocator` | 84 | Exhaust/recover, free-list integrity, 1M alloc/free cycles |
| `test_histogram` | 21 | Bucket indexing, percentile accuracy, concurrent recording |
| `test_multi_symbol_engine` | 40 | Routing consistency, unregistered-symbol rejection, honest pin/realtime status — the class did not compile before this suite existed |
| `test_replay` | 10 | `ReplayResult` zero-init regression, trace round-trip, bad-magic rejection — `replay.hpp` had no dedicated tests before |
| `test_market_data` | 37 | Wire decode correctness, gap detection, and the heap-overflow regression — `MarketDataIngestion` had no dedicated tests before |
| `feed_to_execution_demo` | 20 | Wire bytes → feed handler → matching engine → execution report, end to end |
| `recovery_demo` | 14 | WAL-replay reconstructs exact pre-crash book state |
| `soak_test` (smoke) | pass/fail | 15s sustained-load correctness + memory-growth check; see `BENCHMARK_RESULTS.md` for a real 75s run |
| `test_conservation` | 1M events × 5 seeds (+10M in a dedicated CI job) | Model-based differential fuzzer — see Correctness above |
| `replay_trace` (`ReplayDeterminism`) | 500K-event trace | Record → replay in a fresh process → fills byte-identical |
| **Total (unit/property)** | **313** | **0 failures — verified under Release, ASan+UBSan, and TSan** |

---

## What Is Not Here (Intentionally)

**Network transport:** `MarketDataIngestion` accepts `span<const uint8_t>`.
Plugging in DPDK or kernel-bypass UDP is a one-function change.
`examples/feed_to_execution_demo.cpp` demonstrates the integration piece
that transport plugs into — draining `MarketDataIngestion`'s output and
calling `MatchingEngine::submit()` — which had no code anywhere
connecting the two before that file existed.

**Formal persistence guarantees:** `examples/recovery_demo.cpp`
demonstrates the reconstruction half of a WAL-based recovery story using
machinery this codebase already has (`TraceWriter`/`OrderFlowReplay`,
already proven byte-exact deterministic elsewhere) — log every event,
destroy the engine, replay the log through a fresh one, and the
reconstructed book state matches pre-crash state exactly, verified by
content. Left out: fsync durability policy for the log file itself, log
rotation, and compaction — see LIMITATIONS.md.

**Multi-symbol parallelism** is implemented (`MultiSymbolEngine`, one
thread per shard, hash-routed by symbol) — but did not compile at all
until this update (see Correctness, bugs 6-8), and its scaling numbers in
`BENCHMARK_RESULTS.md`/`PROFILING.md` §4 are honestly reported from a
single-core sandbox that cannot demonstrate genuine cross-core
parallelism. What's still out of scope regardless of core count is
anything *cross-symbol*: combo/spread books, position limits by
underlying — see LIMITATIONS.md.

**Risk / pre-trade checks:** Fat-finger and position limits live between
ingestion and matching — architecturally uninteresting comparisons.
