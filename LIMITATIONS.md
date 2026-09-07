# Limitations

Scope is deliberate. Listed here so nobody has to discover it.

- **Single-instrument core.** `MultiSymbolEngine` shards books by symbol
  onto threads, but there is no cross-symbol logic: no combo/spread books,
  no position limits by underlying, nothing options-specific.
  `MultiSymbolEngine` itself did not compile at all until this was
  written — it called two methods (`SPSCQueue::pop`, `MatchingEngine::
  on_message`) that don't exist, uncaught because nothing anywhere in
  this codebase ever instantiated the class template and called
  `start()` on it (a class template's member function bodies are only
  checked when actually used). Its routing was also internally
  inconsistent even once the method names were fixed — `register_symbol`
  placed a symbol by hashing its ticker string; `submit` picked a shard
  by masking the message's raw integer id, an unrelated computation.
  Both fixed; `tests/test_multi_symbol_engine.cpp` (40 assertions) is the
  regression coverage, including a test that specifically confirms
  traffic for each registered symbol stays on the shard it was assigned
  to at registration.
- **Fixed capacity.** 4,096 price levels and 65,536 live orders per book.
  Beyond that, `add_order` rejects, and does so cleanly — without leaving
  an orphaned empty price level behind (see the "bugs found" list in the
  README's Correctness section).
- **No network, formal durability guarantees, or risk checks.** Ingress
  is an in-process SPSC queue; `examples/feed_to_execution_demo.cpp`
  demonstrates the missing wiring between a wire-format feed handler
  (`MarketDataIngestion`) and the matching engine end to end (nothing
  connected the two before that file existed — each had separate test
  coverage, but no code anywhere drained one's output queue into the
  other's `submit()`). Building that integration also surfaced a real,
  ASan-confirmed heap-buffer-overflow READ in `MarketDataIngestion::
  ingest()`: payload-length validation happened after constructing a
  `std::span::subspan()` from unvalidated input, and that call is
  undefined behavior (not clamped) when the requested count exceeds the
  actual buffer — a truncated or malformed wire message could read past
  the end of a real allocation. Fixed by validating the raw buffer size
  before constructing the subspan at all; see `market_data.cpp`'s
  `ingest()` for the full explanation. "No risk checks" still stands —
  this fix is about safely REJECTING malformed input, not about
  evaluating whether well-formed input should be allowed through.
- **Recovery is demonstrated, not productionized.** `examples/
  recovery_demo.cpp` proves the write-ahead-log story: `TraceWriter`/
  `OrderFlowReplay` (already used for latency benchmarking and already
  proven byte-exact deterministic by `tools/replay_trace.cpp`) can log
  every order event as it happens and, after destroying the engine to
  simulate a crash, reconstruct EXACTLY the pre-crash book state
  (verified by content — best bid/ask — not just "didn't crash") by
  replaying that log through a fresh engine. What's still genuinely
  absent: a durability guarantee for the trace file itself (no fsync
  policy, no defined behavior for a crash mid-write to the log), log
  rotation, and compaction.
- **Modify preserves price-time priority correctly.** `modify_order` edits
  quantity in place and keeps queue position on a decrease. On an increase
  it unlinks the order and re-appends it at the tail of the same price
  level's FIFO, so it loses time priority — matching real exchange
  semantics. Verified by `test_modify_increase_loses_priority` /
  `test_modify_decrease_keeps_priority` in `test_order_book.cpp`, and by
  the model-based fuzzer (`test_conservation.cpp`), which now exercises
  both directions and checks the exact fill sequence, not just quantities.
- **Cancel is O(1)** via a doubly-linked intrusive list (`slots_.prevs[]`).
  The implementation was verified by the model-based property test
  (`test_conservation.cpp`) which caught two bugs in the first attempt.
- **Hash index is linear-probe with backward-shift**, not Robin Hood.
  Expected probe length 1.5 at 50% load; degrades under adversarial keys.
- **Benchmarks now cover realistic order flow and real multi-symbol
  execution, still on shared/container hardware.** `bench_replay`
  generates Modify traffic (previously absent entirely), bursty
  inter-arrival timing (previously a flat uniform rate), and multiple
  symbols — its "naive `std::map` reference" comparison also used to read
  the trace file with the wrong on-disk struct layout entirely,
  reporting 0 fills out of 500,000 events and a fabricated throughput
  figure; fixed, and the real naive-baseline number is meaningfully
  slower than this engine, not faster as the old broken comparison
  implied. `bench_multisymbol` now drives the real, fixed
  `MultiSymbolEngine` (it previously reimplemented sharding by hand,
  never touching the actual class) — see `BENCHMARK_RESULTS.md` for the
  numbers, all real and machine-labeled, none of it hides that this
  remains a single-core sandbox (`nproc` reports 1), which caps how much
  the multi-shard numbers can show.
- **Self-trade prevention is opt-in and deliberately conservative.**
  `Order::account_id` defaults to 0, which never triggers STP — every
  existing caller (tests, benchmarks, `MatchingEngine`) is unaffected. Set
  it on both sides and, when the aggressor would trade against its own
  resting order, matching stops at that order rather than skipping past
  it — skipping would jump a same-account order ahead of a different
  account's order still queued behind it, which is a worse violation than
  not implementing STP at all. Since matching stopped there, the
  aggressor's remainder is cancelled rather than rested (resting it would
  leave the book crossed against the price it just refused to trade at).
  It does **not** implement "skip and match the next order" or "cancel
  the resting order" policies real venues also offer. Covered by
  `test_self_trade_prevention` and by the fuzzer (`test_conservation.cpp`
  assigns ~30% of generated orders one of 8 account ids).
- **No isolated-core numbers recorded yet — and this environment cannot
  produce them.** Everything in `BENCHMARK_RESULTS.md` is from a shared
  container reporting exactly 1 CPU core; true core isolation
  (`isolcpus`, a dedicated core with nothing else scheduled on it) is not
  something a single-core machine can demonstrate regardless of
  configuration. What WAS genuinely measurable here: `SCHED_FIFO`
  scheduling-class behavior in isolation from core dedication —
  and the result is a real, useful finding, not a null one. Two
  busy-poll `SCHED_FIFO` threads sharing one core split scheduled
  iterations roughly 8,800:1 in a direct measurement, which is exactly
  why `MatchingEngine::start()` now refuses to apply `SCHED_FIFO` unless
  a real core pin was also requested (`cpu_id >= 0`) — see
  `docs/design.md` §5(d). A soak test (`tools/soak_test.cpp`) exists for
  sustained-load correctness and memory-growth checking; it deliberately
  runs with `cpu_id = -1` for the same reason.
- **No hardware performance-counter data (cache-miss/branch-miss rates).**
  `perf_event_open()` fails at the syscall level in this sandbox
  (`ENOENT`) — not a missing CLI tool or a kernel-version mismatch in the
  `perf` binary specifically, the underlying counter subsystem itself is
  unavailable in this virtualized environment. `PROFILING.md` documents
  what was actually attempted and what it would take to get real numbers
  (any bare-metal Linux host, or a VM configured to pass through the
  host's PMU).
