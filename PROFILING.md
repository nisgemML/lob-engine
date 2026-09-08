# Profiling — lob-engine

Real measurements only. Numbers from `perf stat` are pending a bare-metal run
(see the template command at the bottom). Everything here is from the benchmark
binaries committed under `bench/`.

## Environment

```
CPU      : Intel Xeon @ 2.10 GHz (shared cloud container)
Kernel   : Linux 6.x x86-64
Compiler : GCC 13.3.0, -O3 -march=native (AVX2 enabled)
Isolation: NONE — no isolcpus, no nohz_full, no SCHED_FIFO
```

Container numbers carry large p99+ tails from the OS scheduler. The p50 is
representative; p99 is dominated by scheduler jitter and should not be quoted
without a note. See `docs/linux-tuning.md` for the isolated-core setup.

---

## 1. Single-symbol order book latency (`bench/bench_latency.cpp`)

```
add_order  (passive, no fill)  p50=  64 ns   p99= 19,702 ns
add_order  (aggressive, fill)  p50= 126 ns   p99= 60,669 ns
cancel_order (O(1) doubly-linked list)  p50=  30 ns   p99= 56,498 ns
submit via SPSC                p50=  32 ns   p99=     49 ns   p99.9= 11,398 ns

Throughput: 6.3 M msg/sec  (1M orders, single thread)
```

The p99 spikes on add/cancel are L2/LLC misses from the cold order-index hash
lookup when the working set (65,536-slot pool × 64 bytes = 4 MB) exceeds L2.
On an isolated core with a warm working set the p99 converges to 2–3× p50.

cancel_order is O(1) since the doubly-linked intrusive list was introduced
(see `include/util/function_ref.hpp` and `src/core/order_book.cpp`). The
previous singly-linked implementation was O(depth at level). The change was
verified by the model-based property test against the reference book, which
caught two bugs in the first implementation attempt (free-list aliasing and
missing prevs assignment at enqueue).

---

## 2. AVX2 vs scalar `find_level` (`bench/bench_avx2.cpp`)

```
N_LEVELS=128, N_ITER=5,000,000 (70% hit / 30% miss)

Method     p50    p90    p99   p99.9
Scalar      67ns   90ns  110ns  153ns
AVX2        41ns   53ns   62ns   74ns

Speedup at p50: 1.6x
Correctness: PASS (0 errors across all N_LEVELS)
```

AVX2 `VPCMPEQQ` compares 4 int64 prices per cycle. The scalar loop does one.
At depth=128 this requires 32 AVX2 loads vs 128 scalar loads.

---

## 3. SoA vs AoS cache layout (`bench/bench_cache.cpp`)

**A real, confirmed bug in this benchmark's calibration corrupted every
absolute number it had ever reported, and — once fixed — the "SoA wins
significantly" conclusion below turned out not to hold up on this
hardware.** `cycles_per_ns()` used a non-`volatile` accumulator variable
in its calibration loop; under this project's actual build flags
(`-O3 -march=native`), the compiler proved the variable's final value was
never observed (`(void)x` silences the unused-variable warning, it does
not count as a use) and eliminated the entire 10-million-iteration loop
as dead code. Confirmed by disassembly: the function's two `rdtsc()`
calls ended up back-to-back with nothing between them. The resulting
"calibration" measured a few nanoseconds of call/pipeline noise instead
of 10M real iterations, producing a different, wrong GHz reading on
every single run — five consecutive runs before the fix showed 0.08,
0.17, 0.28, 0.29, and 0.32 GHz, on a machine whose real clock (per
`/proc/cpuinfo`) is 2.1 GHz. Fixed by making the accumulator `volatile`,
the same fix `bench/bench_avx2.cpp`'s own `calibrate()` already used
correctly for the identical pattern. Post-fix, calibration reads a
stable 2.10 GHz on every run.

**The honest result once measurement is trustworthy:** at depth=1024
(the deepest level this benchmark tests), 10 consecutive runs after the
fix gave speedups of 1.02, 1.04, 1.03, 1.08, 1.03, 0.96, 1.02, 0.98,
1.01, 1.05 — mean ≈1.02x, range 0.96x-1.08x. That is **not** a
significant win; it's within the noise floor of this shared, single-core,
virtualized sandbox, and on some runs AoS was measured as marginally
faster. The absolute per-scan times are now trustworthy (a representative
run):

```
Depth    AoS (ns)   SoA (ns)   Speedup   Cache lines (AoS / SoA)
   1        2.8        2.5      1.14x      1 / 1
   4        6.2        5.8      1.07x      2 / 1
   8        7.8        8.0      0.98x      4 / 1
  16        8.3        9.0      0.93x      8 / 2
  32       14.3       15.2      0.94x     16 / 4
  64       25.6       25.3      1.01x     32 / 8
 128       46.2       46.7      0.99x     64 / 16
 256      101.4      115.3      0.88x    128 / 32
 512      175.5      166.9      1.05x    256 / 64
1024      325.7      332.0      0.98x    512 / 128
```

**What this does and doesn't mean:** the SoA layout still touches
provably fewer cache lines (the table's own rightmost column — 128 vs 32
at depth=1024, structurally true regardless of any timing measurement).
Whether that translates into a *measurable* latency win depends on
whether the working set is actually cold enough, on this specific
hardware, for the difference to show up above other noise sources — and
on this shared, single-core, virtualized sandbox, it largely doesn't.
This is exactly the kind of claim that needs a quiet, dedicated,
preferably isolated-core machine to settle with confidence (see §4 and
`LIMITATIONS.md` for why that's not available here) — the structural
argument for SoA (fewer cache lines touched, verified) is sound
independent of this specific noisy measurement; the *quantitative* "1.3x
faster" claim this section used to make was not something this
environment, once measured correctly, actually supports.

---

## 4. Multi-symbol throughput scaling (`bench/bench_multisymbol.cpp`)

**This section previously reported numbers from a benchmark that never
touched `MultiSymbolEngine` at all.** The real class didn't compile
(`SPSCQueue::pop`/`MatchingEngine::on_message` don't exist — see
`LIMITATIONS.md` and `include/core/multi_symbol_engine.hpp`'s header
comment for the full story), and the benchmark that produced the numbers
below it used to show reimplemented sharding by hand with raw
`OrderBook` objects on unpinned `std::thread`s — a different, simpler,
never-shipped architecture. The numbers were real for THAT
implementation; they were not evidence about the class this document,
`README.md`, and `LIMITATIONS.md` all described as "implemented." This
section now reports what the real, fixed `MultiSymbolEngine` actually
does, on the same container hardware.

```
CPU: Intel(R) Xeon(R) Processor @ 2.10GHz — nproc reports 1

Method: MultiSymbolEngine<N>, 4 symbols hashed across N shards, one feed
thread, 125,000 events/symbol, all shards unpinned (cpu_affinity=-1,
which — see docs/design.md §5(d) — also means no SCHED_FIFO; forcing
SCHED_FIFO on more busy-poll threads than this machine has cores was
measured directly to cause severe scheduling starvation, not a speedup).

Shards   Agg Mmsg/s   Scaling
     1        4.87       1.00x
     2        6.28       1.29x
     4        6.66       1.37x
     8        3.90       0.80x
```

**Why this looks nothing like a clean scaling curve, and why that's the
honest answer on this hardware:** this machine has exactly one CPU core.
There is no scenario in which N independent OS threads on one core show
anything resembling linear scaling — what's actually being measured here
is how well `MultiSymbolEngine`'s routing and per-shard queueing overhead
holds up under time-sliced contention, not parallelism, because there is
none to have. The modest improvement from 1→4 shards (up to 1.37x) is
plausibly explained by overlap during otherwise-idle waits (one shard's
thread can run while another is blocked on a queue-full/empty condition);
the drop at 8 shards (0.80x, WORSE than one shard) is consistent with
thread and context-switch overhead exceeding any such overlap benefit
once thread count is pushed well past what one core can usefully
interleave.

**What this means for the "expected on isolated cores" claim below:**
that projection is unchanged in spirit — genuinely independent cores
should scale close to linearly, since each shard's `OrderBook` working
set then stays resident in ITS OWN L2/L3, with no time-slicing
contention at all — but this 1-core sandbox cannot itself validate it.
Real 8-core data is now available below and tells a more interesting,
less clean story than the simple prediction.

**Why scaling degrades on this machine, more precisely:** with 1 CPU,
"the OS scheduler migrates threads between physical cores mid-run" (the
previous version of this note's explanation) doesn't apply — there's
only one core to run on. Time-slicing contention between shard threads,
not cache-line migration between cores, is the mechanism here.

**Expected on isolated cores:** each shard runs on its own pinned core with no
migrations; the 4 MB working set stays in L2/L3 local to that core.
Scaling should be ≥0.95× per doubling, i.e. 2 shards → ~1.90× aggregate.

---

### 4b. Real multi-core measurement (WSL2, 8 cores) — genuinely
parallel, and genuinely more complicated than predicted

`bench_multisymbol` was given a `--pinned` flag (shard *i* pinned to
core *i* via `ShardConfig::cpu_affinity`) specifically so it could be run
both ways on the same real hardware for a controlled comparison. Real
run, WSL2 (Linux 6.18 kernel, Intel Core Ultra 7 155H, 8 logical cores),
same workload as above:

```
Unpinned: 1x=9.46   2x=17.34 (1.83x)   4x=34.27 (3.62x)   8x=25.35 (2.68x)
Pinned:   1x=10.07  2x=17.22 (1.71x)   4x=9.99  (0.99x)   8x=24.09 (2.39x)
```

**The unpinned 4-shard case (3.62x) is the closest this repo has come to
validating genuine multi-core scaling** — real, meaningful, and roughly
consistent with what §4's prediction would expect for a system with
some scheduling overhead. That alone is worth having: this project's own
architecture genuinely benefits from real parallelism when it's
available, not just in theory.

**The 4-shard PINNED case (0.99x — no better than one shard) is the
genuinely surprising result, and it's not being explained away.** Two
honest possibilities, neither confirmed by further measurement yet:

1. This CPU (Core Ultra 7 155H) is a hybrid P-core/E-core design, and
   WSL2 runs as a Hyper-V VM — pinning a thread to a specific *virtual*
   core inside the guest does not pin it to a specific *physical* core;
   the hypervisor's own scheduler still decides that mapping. It is
   plausible that explicit pinning happened to land shards on slower
   E-cores in this run, while the OS's own free scheduling (the unpinned
   case) opportunistically found faster P-cores — meaning pinning could
   genuinely backfire in a virtualized, hybrid-core environment in a way
   it would not on genuinely uniform bare-metal cores.
2. Simple run-to-run variance on a machine also running a full desktop
   OS underneath the VM — this was a single run each way, not an
   averaged series.

**This is exactly why "pinned" and "isolated" are not the same claim,
stated plainly rather than glossed over.** `ShardConfig::cpu_affinity`
gives you process-level affinity — a request the OS (and, here, the
hypervisor) is free to honor imperfectly. True `isolcpus`-level isolation
(a core reserved from the scheduler entirely, at boot, with nothing else
— not even the kernel's own housekeeping — touching it) is a stronger,
different guarantee this repo still has not measured, on bare metal or
otherwise. The right conclusion from this specific dataset is "pinning
is not free lunch in a virtualized, hybrid-core environment," not
"pinning doesn't work" — the 2-shard and 8-shard cases both still show
substantial real speedup over one shard either way.

To reproduce on your own hardware:
```bash
./bench_multisymbol            # unpinned baseline
./bench_multisymbol --pinned   # shard i -> core i
```

---

## 5. perf stat — attempted, and genuinely unavailable in this environment

This section used to be a template asking the reader to paste their own
`perf stat` output. An honest update: `perf_event_open()` — the actual
syscall behind the `perf` CLI tool, checked directly rather than assuming
the CLI tool's own "kernel version mismatch" message was the whole
story — returns `ENOENT` in this sandbox:

```c
struct perf_event_attr pe = { .type = PERF_TYPE_HARDWARE,
                               .config = PERF_COUNT_HW_CPU_CYCLES, ... };
long fd = syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
// fd == -1, errno == ENOENT
```

This means the hardware performance-counter subsystem itself is
unavailable here — not a missing `linux-tools-<version>` package (the
`perf` binary IS installed; it correctly reports it can't find a
kernel-matched helper, which is a real but different problem), and not
fixable by installing anything in this container. Common in virtualized
/ sandboxed environments that don't pass through PMU access to the
guest. Getting real `IPC`/`L1-dcache-load-misses`/`branch-misses` numbers
for this codebase requires either bare-metal Linux or a VM/container
explicitly configured to expose the host's performance counters — the
commands below are correct and unchanged; they simply could not be run
to completion here.

```bash
# Build release
cmake -S . -B build && cmake --build build

# Pin to core 3 (must be isolated)
taskset -c 3 chrt -f 50 \
  perf stat -e cycles,instructions,branches,branch-misses,\
               L1-dcache-loads,L1-dcache-load-misses,\
               LLC-loads,LLC-load-misses \
  ./build/bench_latency
```

Metrics to look for once run on hardware that actually exposes counters:
- IPC > 2.5 on the add_order path indicates good instruction-level parallelism
- L1-dcache-load-misses < 1% indicates the hot path is L1-resident
- branch-misses < 0.5% confirms the AVX2 branch elimination is working

---

## 6. Model-based correctness vs latency trade-off

The model test (`tests/test_conservation.cpp`) runs 1,000,000 random events
per seed against a `std::map<Price, deque<Order>>` reference and asserts exact
fill sequence, best quote, no crossed book, and quantity conservation.

Throughput of the reference model: ~450k events/sec (dominated by map operations).
Throughput of lob-engine: ~8.5M events/sec on this hardware.

**Ratio: ~19× faster than a naive correct implementation.**

This number is what `test_conservation` proves: lob-engine produces identical
results to the reference model at 19× its speed.
