#pragma once
// include/core/multi_symbol_engine.hpp — Multi-symbol matching engine.
//
// ═══════════════════════════════════════════════════════════════════════════
// DESIGN
// ═══════════════════════════════════════════════════════════════════════════
//
// The single-symbol MatchingEngine is optimal for one symbol on one thread.
// Production options systems have thousands of strikes per underlying, each
// requiring its own order book. The multi-symbol engine shards across cores.
//
// Architecture:
//
//   Symbols → hash → shard (one per CPU core)
//                      ↓
//   Each shard: one MatchingEngine (owns its own thread, queues, and books)
//
// This class does not run its own threads or own its own SPSC queues — a
// MatchingEngine already owns exactly one of each internally. A shard is
// just "one MatchingEngine, told which symbols route to it and which CPU
// to pin its thread to." An earlier version of this file duplicated a
// queue and a thread per shard on top of that (Shard::queue, Shard::thread,
// shard_loop()) and drove them by calling shard->queue.pop(msg) and
// shard->engine.on_message(msg) — neither of which exists on SPSCQueue or
// MatchingEngine (the real names are try_pop() and submit()). Because this
// is a class template and nothing anywhere in this codebase ever
// instantiated MultiSymbolEngine and called start() on it, the compiler
// never had a reason to fully check shard_loop()'s body — a class
// template's member function bodies are only instantiated (and thus fully
// type-checked) when actually called, so this went uncaught: the class
// documented in README.md/LIMITATIONS.md as "implemented" would not
// compile the moment anyone actually used it. Confirmed directly by
// writing a one-file program that instantiates MultiSymbolEngine<4> and
// calls register_symbol()/start(): two hard compile errors, exactly where
// expected. Rewritten below to delegate everything to MatchingEngine's own
// (already-tested) threading, queueing, and now-configurable CPU pinning
// instead of re-implementing any of it.
//
// A second, subtler bug in the same file: register_symbol() placed a
// symbol by hashing its TICKER STRING (fnv1a(symbol) & mask), but
// submit() picked a shard by masking the message's raw integer SymbolId
// directly (msg.symbol & mask) — two unrelated numbers. Even with the
// method names fixed, a message would very likely be routed to a shard
// that never registered that symbol at all. Fixed by recording the shard
// each SymbolId was actually placed on at registration time and routing
// by table lookup at submit time — the same decision, reused, not a
// second computation that has to happen to agree with the first one.
//
// Design decisions:
//
// 1. HASH-BASED SHARDING (not sorted): options symbols (AAPL230915C00150000)
//    do not have natural ordering for round-robin. FNV hash of ticker gives
//    uniform distribution across shards.
//
// 2. NO CROSS-SHARD COMMUNICATION on the matching hot path: each shard's
//    MatchingEngine has its own inbound/outbound SPSC queues, submitted to
//    and drained independently.
//
// 3. CPU PINNING: each shard's MatchingEngine is started with its own
//    cpu_id, pinning that shard's thread to a dedicated core via
//    MatchingEngine::start()'s (now checked, not assumed) pthread_setaffinity_np.
//
// 4. NUMA AWARENESS (stub): for systems with multiple NUMA nodes, shards
//    should be allocated on the same node as their CPU. Implemented as a
//    documented stub — requires NUMA-aware allocator (libnuma or hwloc).
//
// ── Scaling ───────────────────────────────────────────────────────────────
//
// At 4 shards: 4× throughput, 4× total capacity, assuming genuinely
// isolated cores — see PROFILING.md §4 for what this repo could and could
// not measure on a shared, single-core sandbox, and bench_multisymbol.cpp
// for the real numbers this class now produces when actually exercised.
// Cross-shard spread orders (e.g., buy C + sell P on same underlying)
// require a coordinator — implemented as a documented stub here.
//
// ═══════════════════════════════════════════════════════════════════════════

#include "core/matching_engine.hpp"
#include "core/types.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace engine {

// ── FNV-1a hash for symbol strings ────────────────────────────────────────
[[nodiscard]] inline uint32_t fnv1a(std::string_view s) noexcept {
    uint32_t h = 2166136261u;
    for (char c : s) { h ^= uint8_t(c); h *= 16777619u; }
    return h;
}

// ── ShardConfig ───────────────────────────────────────────────────────────
struct ShardConfig {
    int cpu_affinity = -1;     // -1 = no pinning (see MatchingEngine::start())
    int numa_node    = -1;     // -1 = no NUMA preference
};

// ── MultiSymbolEngine ─────────────────────────────────────────────────────
//
// N_SHARDS must be a power of 2 for hash-based routing.
//
template <std::size_t N_SHARDS = 4>
class MultiSymbolEngine {
    static_assert((N_SHARDS & (N_SHARDS - 1)) == 0, "N_SHARDS must be power of 2");

public:
    using ExecCallback = std::function<void(ExecutionReport const&)>;

    // ── Shard ─────────────────────────────────────────────────────────────
    //
    // Just a MatchingEngine plus which CPU it should pin to and a
    // drop counter for submissions this shard's queue couldn't accept
    // (MatchingEngine::submit() reports success/failure per call but
    // doesn't itself keep a running count — that bookkeeping belongs to
    // whoever's routing to it, which is this class).
    struct Shard {
        alignas(64) MatchingEngine engine;
        int cpu_id = -1;
        alignas(64) std::atomic<uint64_t> msgs_dropped{0};
    };

    // ── Construction ──────────────────────────────────────────────────────
    explicit MultiSymbolEngine(
        ExecCallback       on_exec,
        ShardConfig const* shard_configs = nullptr)
        : on_exec_{std::move(on_exec)}
    {
        symbol_to_shard_.fill(-1);
        for (std::size_t i = 0; i < N_SHARDS; ++i) {
            shards_[i] = std::make_unique<Shard>();
            if (shard_configs) {
                shards_[i]->cpu_id = shard_configs[i].cpu_affinity;
            }
        }
    }

    ~MultiSymbolEngine() { stop(); }

    // ── register_symbol: assign symbol to a shard ─────────────────────────
    //
    // Must be called before start(). The shard is chosen once, here, by
    // hashing the ticker string — submit() below routes every subsequent
    // message for this SymbolId to the SAME shard via table lookup, not by
    // re-deriving a shard index from the message some other way.
    bool register_symbol(std::string_view symbol, SymbolId id) {
        if (id >= MatchingEngine::kMaxSymbols) return false;
        const std::size_t shard_idx = fnv1a(symbol) & (N_SHARDS - 1);
        if (!shards_[shard_idx]->engine.register_symbol(id)) return false;
        symbol_to_shard_[id] = int(shard_idx);
        return true;
    }

    // ── start: launch all shard threads ───────────────────────────────────
    //
    // Delegates to each shard's own MatchingEngine::start(cpu_id) — that
    // is where the thread, the pinning, and the SCHED_FIFO elevation
    // actually happen; there is no separate thread or queue at this level
    // anymore. See is_shard_pinned()/is_shard_realtime() to check whether
    // a given shard's pin/SCHED_FIFO request actually succeeded, per
    // MatchingEngine's own now-checked start().
    void start() {
        for (std::size_t i = 0; i < N_SHARDS; ++i) {
            shards_[i]->engine.start(shards_[i]->cpu_id);
        }
    }

    // ── stop: drain queues and join threads ───────────────────────────────
    void stop() {
        for (auto& shard : shards_) {
            if (shard) shard->engine.stop();
        }
    }

    // ── submit: route message to the correct shard ────────────────────────
    //
    // Called from the feed handler / ingestion thread. Lock-free: pushes
    // to the target shard's MatchingEngine's own inbound SPSC queue.
    //
    // Returns false if the symbol was never registered, or if the target
    // shard's queue is full.
    [[nodiscard]] bool submit(MarketDataMsg const& msg) noexcept {
        if (msg.symbol >= symbol_to_shard_.size()) return false;
        const int shard_idx = symbol_to_shard_[msg.symbol];
        if (shard_idx < 0) return false;  // unregistered symbol
        auto* shard = shards_[std::size_t(shard_idx)].get();
        if (shard->engine.submit(msg)) return true;
        shard->msgs_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // ── poll_reports: drain and dispatch execution reports from every
    // shard ────────────────────────────────────────────────────────────────
    //
    // MatchingEngine itself is externally-polled (see its own
    // poll_report()) rather than callback-driven — this class follows the
    // same model instead of spinning one extra thread per shard just to
    // turn polling into a callback. Call this periodically from whichever
    // thread should observe fills (it does not need to be, and generally
    // should not be, any shard's own matching thread). Returns the total
    // number of reports dispatched to on_exec() across all shards in this
    // call.
    std::size_t poll_reports(std::size_t max_per_shard = 1024) noexcept {
        std::size_t dispatched = 0;
        ExecutionReport rpt;
        for (auto& shard : shards_) {
            for (std::size_t n = 0; n < max_per_shard; ++n) {
                if (!shard->engine.poll_report(rpt)) break;
                if (on_exec_) on_exec_(rpt);
                ++dispatched;
            }
        }
        return dispatched;
    }

    // ── Statistics ────────────────────────────────────────────────────────
    struct Stats {
        std::size_t shard_id;
        uint64_t    msgs_processed;
        uint64_t    msgs_dropped;
        int         cpu_id;
        bool        pinned;     // did this shard's pin request actually succeed?
        bool        realtime;   // did this shard's SCHED_FIFO request actually succeed?
    };

    std::vector<Stats> stats() const {
        std::vector<Stats> result;
        result.reserve(N_SHARDS);
        for (std::size_t i = 0; i < N_SHARDS; ++i) {
            result.push_back({
                i,
                shards_[i]->engine.messages_processed(),
                shards_[i]->msgs_dropped.load(std::memory_order_relaxed),
                shards_[i]->cpu_id,
                shards_[i]->engine.is_pinned(),
                shards_[i]->engine.is_realtime(),
            });
        }
        return result;
    }

    static constexpr std::size_t n_shards() noexcept { return N_SHARDS; }

    // Direct access to a shard's engine — useful for tests/benchmarks that
    // want to submit/poll a specific shard without going through the
    // hash-based routing (e.g. to construct a worst-case single-shard load).
    [[nodiscard]] MatchingEngine& shard_engine(std::size_t i) noexcept {
        return shards_[i]->engine;
    }

private:
    // ── NUMA-aware shard allocation (stub) ────────────────────────────────
    //
    // Production implementation:
    //   #include <numa.h>
    //   void* mem = numa_alloc_onnode(sizeof(Shard), numa_node);
    //   new (mem) Shard{};
    //
    // This ensures shard data is on the same NUMA node as its CPU, avoiding
    // cross-NUMA memory access (~40ns extra per cache miss on dual-socket).
    //
    // Required: libnuma (apt install libnuma-dev) and build with -lnuma.

    std::array<std::unique_ptr<Shard>, N_SHARDS>            shards_;
    std::array<int, MatchingEngine::kMaxSymbols>            symbol_to_shard_{};
    ExecCallback on_exec_;
};

}  // namespace engine
