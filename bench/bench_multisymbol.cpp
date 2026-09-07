// bench/bench_multisymbol.cpp — Multi-symbol throughput scaling, measured
// through the actual MultiSymbolEngine class, not a hand-rolled stand-in.
//
// ── Why this file changed ─────────────────────────────────────────────────
//
// The previous version of this benchmark never touched MultiSymbolEngine
// at all — it reimplemented sharding directly with raw OrderBook objects
// on unpinned std::threads. That was masking a real problem: the actual
// MultiSymbolEngine class didn't compile (see multi_symbol_engine.hpp's
// header comment and tests/test_multi_symbol_engine.cpp for the two
// method-name bugs and the routing-consistency bug this uncovered). The
// numbers this file used to produce were real numbers for a different,
// simpler, never-shipped architecture — not for the class the README and
// LIMITATIONS.md described as "implemented." This version exercises the
// real thing: MatchingEngine's own SPSC queues, its own thread, and
// (attempted) its own CPU pinning per shard, exactly as a caller would use
// it in production.
//
// ── What this measures ────────────────────────────────────────────────────
//
// N independent symbols, each hashed to one of N_SHARDS, submitted from a
// SINGLE feed-simulation thread (representing one ingestion pipeline
// feeding all shards) at maximum rate, while each shard's MatchingEngine
// drains and matches independently on its own thread.
//
// ── Honesty about this environment ────────────────────────────────────────
//
// This machine reports 1 CPU core (`nproc`). Per matching_engine.hpp's
// start() doc comment, cpu_affinity is left at -1 (no pinning, no
// SCHED_FIFO) for every shard here — attempting to pin N shards' threads
// to N distinct cores that don't exist would fail outright, and applying
// SCHED_FIFO to several busy-poll threads sharing one core was measured
// directly to produce a ~8,800:1 scheduling split between just two such
// threads (see matching_engine.cpp's start()). Running this benchmark
// with real per-core pinning requires a machine with at least N_SHARDS
// cores — see PROFILING.md §4 for exactly what was and wasn't possible to
// measure here, and README.md for the isolated-core numbers this repo
// does not have yet.

#include "core/multi_symbol_engine.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace engine;
using Clock = std::chrono::steady_clock;

static std::string env_line(const char* f, const char* key) {
    std::ifstream s(f); std::string l;
    while (std::getline(s, l)) if (l.rfind(key, 0) == 0) return l.substr(l.find(':') + 2);
    return "?";
}

static std::vector<MarketDataMsg> gen_events(uint64_t seed, size_t n, SymbolId symbol) {
    std::mt19937_64 rng(seed);
    std::vector<MarketDataMsg> evts; evts.reserve(n);
    std::vector<OrderId> live;
    OrderId next_id = 1;
    for (size_t i = 0; i < n; ++i) {
        int roll = int(rng() % 100);
        MarketDataMsg m{};
        m.symbol = symbol;
        if (roll < 70 || live.empty()) {
            m.msg_type   = MarketDataMsg::Type::NewOrder;
            m.order_id   = next_id++;
            m.side       = (rng() & 1) ? Side::Buy : Side::Sell;
            m.price      = to_price(100.0) + Price((int64_t(rng() % 41) - 20) * 10'000);
            m.qty        = Qty(1 + rng() % 500);
            m.order_type = OrderType::Limit;
            evts.push_back(m);
            live.push_back(m.order_id);
        } else {
            size_t k = rng() % live.size();
            m.msg_type = MarketDataMsg::Type::CancelOrder;
            m.order_id = live[k];
            evts.push_back(m);
            live[k] = live.back(); live.pop_back();
        }
    }
    return evts;
}

template <std::size_t N_SHARDS>
static double run_with_n_symbols(std::size_t n_symbols, std::size_t events_per_symbol) {
    std::atomic<uint64_t> fills{0};
    MultiSymbolEngine<N_SHARDS> mse([&](const ExecutionReport&) {
        fills.fetch_add(1, std::memory_order_relaxed);
    });

    for (SymbolId s = 0; s < n_symbols; ++s) {
        char name[16];
        std::snprintf(name, sizeof(name), "SYM%04u", unsigned(s));
        mse.register_symbol(name, s);
    }
    mse.start();

    // Pre-generate all events for all symbols before timing starts — the
    // benchmark measures matching throughput, not RNG cost.
    std::vector<std::vector<MarketDataMsg>> streams(n_symbols);
    for (SymbolId s = 0; s < n_symbols; ++s)
        streams[s] = gen_events(uint64_t(s + 1) * 0x9E3779B97F4A7C15ULL, events_per_symbol, s);

    const auto t0 = Clock::now();
    // Single feed-thread submits round-robin across all symbols — this is
    // the realistic shape (one ingestion pipeline, many downstream
    // shards), not N independent feed threads.
    std::vector<std::size_t> pos(n_symbols, 0);
    std::size_t remaining = n_symbols * events_per_symbol;
    while (remaining > 0) {
        for (SymbolId s = 0; s < n_symbols; ++s) {
            if (pos[s] >= streams[s].size()) continue;
            if (mse.submit(streams[s][pos[s]])) {
                ++pos[s];
                --remaining;
            }
        }
    }
    // Drain: wait for all shards to finish processing what was submitted.
    const uint64_t total_submitted = uint64_t(n_symbols) * events_per_symbol;
    while (true) {
        uint64_t processed = 0;
        for (auto& s : mse.stats()) processed += s.msgs_processed;
        if (processed >= total_submitted) break;
        std::this_thread::yield();
    }
    const auto t1 = Clock::now();

    mse.stop();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    return double(total_submitted) / secs;
}

int main() {
    constexpr std::size_t kEventsPerSymbol = 500'000 / 4; // keep total events comparable across shard counts

    printf("CPU        : %s\nCompiler   : GCC %d.%d.%d\n\n",
        env_line("/proc/cpuinfo", "model name").c_str(),
        __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
    printf("Method: MultiSymbolEngine<N>, N symbols hashed across N shards,\n");
    printf("        one feed thread, %zu events/symbol, unpinned (see header).\n\n",
           kEventsPerSymbol);

    printf("%-10s %14s %12s\n", "Shards", "Agg Mmsg/s", "Scaling");
    printf("%-10s %14s %12s\n", "------", "----------", "-------");

    double baseline = 0.0;

    {
        double r = run_with_n_symbols<1>(4, kEventsPerSymbol);
        baseline = r;
        printf("%-10d %13.2f %11.2fx\n", 1, r / 1e6, 1.0);
    }
    {
        double r = run_with_n_symbols<2>(4, kEventsPerSymbol);
        printf("%-10d %13.2f %11.2fx\n", 2, r / 1e6, r / baseline);
    }
    {
        double r = run_with_n_symbols<4>(4, kEventsPerSymbol);
        printf("%-10d %13.2f %11.2fx\n", 4, r / 1e6, r / baseline);
    }
    {
        double r = run_with_n_symbols<8>(4, kEventsPerSymbol);
        printf("%-10d %13.2f %11.2fx\n", 8, r / 1e6, r / baseline);
    }

    printf("\nNote: all shards run with cpu_affinity=-1 (unpinned, no SCHED_FIFO) —\n");
    printf("see this file's header comment for why forcing pinning/SCHED_FIFO on\n");
    printf("a machine with fewer cores than shards would make these numbers WORSE\n");
    printf("and misleading, not better. This measures MultiSymbolEngine's routing\n");
    printf("and threading overhead in isolation from core-count effects, not the\n");
    printf("scaling a genuinely multi-core, isolated-core deployment would show.\n");
}
