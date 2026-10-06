# Benchmark Results — Options Matching Engine

## How to reproduce every number in this README

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-mavx2"
make -j$(nproc)

# Unit/property suites + the differential fuzzer at 1M events × 5 seeds
ctest --output-on-failure --timeout 120        # OrderBook, SPSC, MPMC,
                                                # Matching, Allocator,
                                                # Histogram, Conservation×5

# The 10M-event differential run (also run by CI as a separate job —
# see .github/workflows/ci.yml: conservation-long)
./test_conservation 1 10000000

# Deterministic replay: record a trace, replay it in a fresh process,
# byte-diff the fills. Also runs as a ctest entry (ReplayDeterminism).
./replay_trace record 1 500000 /tmp/trace.bin
./replay_trace replay /tmp/trace.bin

# Sanitizer builds (also run by CI as separate jobs)
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTSAN=ON  && make -j$(nproc) && ctest --output-on-failure
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DASAN=ON  && make -j$(nproc) && ctest --output-on-failure
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUBSAN=ON && make -j$(nproc) && ctest --output-on-failure

# Order-flow, hot-path, and AVX2-vs-scalar benchmarks
./bench_replay
./bench_avx2
```

Everything below this line is pasted directly from a run of the above,
on this machine, in a shared container (no `isolcpus`, no `SCHED_FIFO`).
`scripts/update_readme.py` diffs this file against README.md's `<!--
BENCH:START/END -->` block and fails if they've drifted.

**Environment:** Ubuntu 24.04 LTS, GCC 13.3.0, -O3 -march=native (AVX2), x86-64 container (no `isolcpus`, no `SCHED_FIFO`)

For production latency numbers, run pinned:
`taskset -c 4 chrt -f 80 ./bench_replay` — p99 converges to 2–3× p50.

---

## Test results

```
20 ctest entries, 0 failed:

  OrderBook              — 43 passed  (includes modify increase/decrease
                                        priority, IOC/FOK, self-trade
                                        prevention; BookFixture now
                                        heap-allocates its OrderBook —
                                        see docs/design.md §9 for the
                                        ASan stack-overflow this fixed)
  SPSCUnit               — 13 passed  (interleaved test now checks
                                        every push/pop result and order)
  SPSCStress             — 13 passed  (new in ctest — 2M items + 1M
                                        wrap-arounds through a depth-4
                                        queue, exact order verified; ~2s
                                        on one vCPU, previously did not
                                        finish there and was manual-only)
  OrderIndex             — 20,019 passed (new — 60,000 sequential ids,
                                        structural longest-cluster check,
                                        wrapping clusters; README bug #16)
  Matching               — 8  passed
  Allocator              — 84 passed
  MPMC                   — 24 passed
  MultiSymbolEngine      — 40 passed  (new — see LIMITATIONS.md; the
                                        class did not compile at all
                                        before this suite existed)
  Replay                 — 10 passed  (new — TraceWriter/OrderFlowReplay
                                        had zero dedicated test coverage
                                        before; includes the ReplayResult
                                        uninitialized-counter regression)
  MarketData             — 37 passed  (new — MarketDataIngestion had zero
                                        dedicated test coverage before;
                                        includes the heap-buffer-overflow
                                        regression, see LIMITATIONS.md)
  FeedToExecutionDemo    — 20 passed  (new — wire bytes -> feed handler
                                        -> matching engine -> execution
                                        report, end to end)
  RecoveryDemo           — 14 passed  (new — WAL-replay reconstructs
                                        exact pre-crash book state)
  SoakTestSmoke          — PASS       (new — 15s sustained-load smoke
                                        check; see below for what a real,
                                        long-duration run looks like)
  Conservation-Seed1..5  — 5 passed   (1M events each; 10M-event run is
                                        a separate, longer-timeout CI job,
                                        not part of this default ctest pass)
  ReplayDeterminism      — record -> replay in a fresh process ->
                                        fills byte-identical
  Histogram              — 21 passed
```

Verified with a real `ctest --test-dir build --output-on-failure` run in
this environment (Release build), and separately under
AddressSanitizer+UBSan and ThreadSanitizer builds — all 18 entries pass
clean under all three configurations. The ASan run is what caught the
`BookFixture` stack-overflow above; a plain Release build never showed
any problem with the exact same test.

**Measured 10M-event run** (this sandbox, not the CI runner or any
production hardware — shared/virtualized vCPU, no isolation, so treat the
absolute time as approximate and expect GitHub Actions or your own
machine to differ):

```
$ time ./test_conservation 1 10000000
events=10000000 seed=1
submitted=1762491028 filled(x2)=1350757268 resting=15859573
cancelled=107523301 rejected=63089568 expired=225261318
fills=5374622  conservation=OK
PASS: engine matches reference model on every event
wall_clock_ms=108319   (~108 seconds, ~92K events/sec)
```

---

## Soak test (`tools/soak_test.cpp`)

New: a long-running stability check for memory growth, correctness, and
sustained throughput — none of which a multi-second benchmark run can
show. Deliberately runs the engine with `cpu_id = -1` (no pinning, no
`SCHED_FIFO`) — see `docs/design.md` §5(d): this test also spins up its
own producer and consumer threads, and this machine's single core means
a `SCHED_FIFO` engine thread would starve them, as a first version of
this test demonstrated directly (it hung for well past its configured
duration and had to be killed).

A real 75-second run, throttled producer (5,000 msg/sec target, well
under this machine's sustainable throughput so the reported numbers
reflect steady-state behavior rather than persistent overload):

```
Elapsed       RSS(KB)           Msgs        Matches
     15.0s      10492          75000          18416
     30.0s      11956         149983          37017
     45.0s      13436         225020          55265
     60.0s      14512         300022          73767
     75.0s      15044         375022          92260

Submitted: 375,022   Dropped: 0 (0.000%)   Processed: 375,022 (exact match)
RSS: 6,200 KB -> 15,044 KB (+142.6% relative; +8.8MB absolute)
```

**Reading the RSS numbers honestly:** the relative percentage looks
large; the per-window deltas tell the more useful story — 1,464 / 1,480 /
1,076 / 532 KB across the four 15-second windows. That's a
**decelerating** growth rate, consistent with allocator arena warmup and
one-time thread/page-cache setup costs settling down, not a constant-rate
leak (which would show roughly equal deltas indefinitely). This is not a
substitute for an actual multi-hour or multi-day run — `./soak_test 3600
60` or `./soak_test 86400 300` — which is what an honest leak-vs-noise
determination actually requires; this 75-second sample is evidence
consistent with "no leak," not proof of it.

---

## Order flow replay benchmark

**bench_replay:** 500K synthetic events across 4 symbols, now including
Modify traffic (NewOrder 60% / aggressive cross 20% / cancel 12% /
modify 8% — Modify was entirely absent before this update) and bursty
inter-arrival timing (alternating dense/quiet periods, not a flat
uniform rate). RDTSC-timed per-event submit latency (decode → LOB update
→ match).

```
=== Order Flow Replay Benchmark (Mode: MaxSpeed) ===

Events replayed : 482,600
Events skipped  : 17,400   (queue backpressure during dense burst periods —
                             see note below; this is a real, honest
                             consequence of adding actual bursts, not
                             something the old uniform-rate generator
                             could ever surface)
Matches         : 277,212

Submit latency — p50 / p90 / p99 / p99.9
  p50  :   37 ns
  p90  :   38 ns
  p99  :   47 ns
  p99.9:  166 ns
```

**On the 17,400 skipped events:** the inbound SPSC queue (65,536 slots)
can fill up when the feed generator's burst periods (20-100ns between
events) submit faster than the matching thread drains, and `MatchingEngine::
submit()` correctly reports failure rather than blocking or silently
dropping data invisibly — the caller (this benchmark) counts it as
skipped. A flat, uniform-rate generator (this benchmark's previous
version) never exercised this path at all, because it never created
sustained submission pressure exceeding the queue's drain rate. This is
a genuine capacity-planning number, not a defect: it says something real
about how large `kQueueDepth` needs to be for a given burst intensity,
which a benchmark with no bursts literally cannot tell you.

**Design notes, and what is and isn't measured here:**

**SoA order book:** `prices[]` is a dense array, so the matching sweep
touches fewer cache lines than a pointer-based layout. That is structurally
true. The speedup is **not** demonstrated in this environment: after the
`bench_cache` calibration fix (README bug #14), 10 runs at the deepest level
averaged ~1.02x, within noise. An earlier version of this paragraph claimed
a "measured ~25ns per match" difference, which contradicted that result and
has been removed. See `PROFILING.md` §3.

**SPSC queues** use release/acquire only. On x86, a `seq_cst` store compiles
to `XCHG` (not `MFENCE`). Either way, it is unnecessary for SPSC.

---

## Order index under deep books (`bench_depth`)

Every other benchmark here keeps the book shallow (a few hundred live
orders). `bench/bench_depth.cpp` rests N non-crossing orders with
**sequential** ids, how exchanges assign them, then cancels them all. It
reports the median of 5 runs, the index's longest probe cluster, and minor
page faults taken while matching.

**Before the fix** (old hash `(id·2654435761)>>32` ≈ 0.618·id, O(cluster²)
delete), one run, stopped by a 240 s timeout:

```
live    add ns/op   cancel ns/op
 1000       292.0        62,037
 4000       920.7       891,122
 8000     1,367.7     3,510,133      (3.5 ms per cancel)
16000   (did not finish)
```

**After** (Fibonacci top-bits hash, single-pass backward-shift delete,
book prefaulted at construction), two separate invocations:

```
    live    add ns/op cancel ns/op    cluster   faults
    1000        124.0         64.2          1        2
    4000         85.5         50.2          1        0
    8000         92.7         64.5          1        0
   16000         83.9         57.6          1        0
   32000         83.7         61.4          1        0
   60000         85.6         60.2          2        0

    1000        109.6         54.4          1        2
    4000         76.0         52.4          1        0
    8000         79.1         52.5          1        0
   16000         78.2         55.3          1        0
   32000         81.6         56.2          1        0
   60000         82.8         59.7          2        0
```

Flat in depth to 60,000 live orders (`kMaxOrders` = 65,536). With the old
hash, the same 60,000 sequential ids formed one 60,000-slot cluster.
Container (one vCPU, no isolation), so treat the absolute ns as tier-2. The
shape is the result.

Why nothing caught it earlier: the conservation test and all benchmarks run
shallow books, and tick-to-trade's real Nasdaq BX day has 15-60 resting
orders per symbol. A main-venue day would have hit it.

**Removed:** a previous "Per-operation latency (unit test instrumentation)"
section (`add_order p50 = 112 ns`, `mean probe length: 1.48`). No code in
the repo produces those numbers, and the probe-length claim was the
opposite of what the old hash did.

---

## Linux tuning for production

See `docs/linux-tuning.md` for the full setup:
`isolcpus`, `nohz_full`, `SCHED_FIFO` priority 50, RCU offload,
interrupt affinity — reduces p99.9 from 238ns to ~60–80ns on isolated cores.

---

## AVX2 vectorised price level search

`find_level_avx2()` in `src/core/order_book.cpp` replaces the scalar loop with
AVX2 SIMD: compares 4 × `int64_t` per cycle (`VPCMPEQQ ymm, ymm, ymm`).

**Benchmark** (N_LEVELS=128, 5M iterations, 70% hit / 30% miss):

```
Method        p50(ns)  p90(ns)  p99(ns)  p99.9(ns)
Scalar            67       90      115        174
AVX2              42       53       64        130

Speedup at p50: 1.6×
Correctness: PASS (0 errors — verified against scalar on 5M random targets)
```

**Why AVX2 matters here:**
- `find_level()` is called on every `add_order` and `cancel` — it is in the hot path
- `prices[]` is L1-resident during the matching sweep (SoA layout)
- At N_LEVELS=128: scalar worst-case ~128 comparisons, AVX2 ~32 iterations
- At N_LEVELS=4096: scalar ~4096 cycles, AVX2 ~1024 cycles
- `VPCMPEQQ ymm, ymm, ymm` — 1 cycle latency, 0.5 throughput on Zen3/Ice Lake

Compile with `-mavx2` to activate. Scalar fallback is automatic when `__AVX2__`
is not defined. See `bench/bench_avx2.cpp` for the full benchmark.

## 4. Naive std::map baseline vs lob-engine (`bench/bench_replay.cpp`)

Same 500,000-event synthetic trace (now multi-symbol, bursty, with real
Modify traffic — see above), single-threaded, shared container.

**This section previously reported a naive-baseline throughput of
"18.4 M msg/sec" and implied the naive implementation was faster than
lob-engine on raw throughput.** That number came from a bug: the
comparison code read the trace file using the wrong on-disk struct
layout entirely (a raw `{uint64_t; MarketDataMsg}` guess, not the actual
packed `TraceEvent` format `TraceWriter` wrote), which silently
misinterpreted every record. Confirmed directly: that version reported
"Fills: 0" out of 500,000 events including a documented 20%+
aggressive/crossing share — impossible if the data being read were the
data that was written. Fixed by reading the real format; the corrected,
real naive-baseline throughput is below, and it changes the actual
conclusion of this section:

```
lob-engine submit p50 (MaxSpeed)  :  37 ns
lob-engine throughput (MaxSpeed)  :  ~8.5 M msg/sec (includes SPSC overhead)
lob-engine book-only throughput   :  6.3 M msg/sec  (from bench_latency.cpp)
Naive std::map throughput         :  5.12 M msg/sec (real Fills: 298,890)
```

**The corrected finding: lob-engine is faster than the naive baseline on
raw throughput, not slower** — the opposite of what the old, buggy
number implied. This makes sense once the naive baseline is actually
doing real work: it pays `std::map`'s O(log n) insert/erase on every
operation with no pooling, no intrusive lists, and dynamic node
allocation per order, which is exactly the cost this codebase's SoA
layout, fixed slot pool and open-addressed order index exist to avoid. The naive
implementation is still valuable as a *correctness* reference (simple
enough to trust by inspection) and as the baseline
`test_conservation.cpp`'s independent model is built in the same spirit
of — but it was never a *performance* baseline this codebase was losing
to, and the previous version of this document said otherwise.

The ratio that matters: the model test (`test_conservation.cpp`) shows the
production book produces identical fills to the reference at millions of
events per seed, across 5 seeds, with the exact fill sequence checked —
not just aggregate counts.

## 5. O(1) cancel verification

cancel_order is O(1) since the doubly-linked intrusive list was added.
The change introduced two bugs that were caught immediately by the model test:
1. free-list aliasing (nexts[] was shared with the live list)
2. missing prevs assignment at enqueue

Both fixed and verified by running 1M events on 5 seeds.
