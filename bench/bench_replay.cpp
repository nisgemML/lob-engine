#include "core/matching_engine.hpp"
#include "core/replay.hpp"
#include "util/histogram.hpp"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <cstring>
#include <chrono>
#include <deque>
#include <map>
#include <unordered_map>
#include <algorithm>

using namespace engine;

// ── Naive std::map reference implementation ──────────────────────────────────
// Used to contextualise the engine's latency numbers.
// The "correct but slow" reference — contextualises the engine's numbers.
struct NaiveBook {
    using PriceMap = std::map<Price, std::deque<std::pair<OrderId,Qty>>>;
    std::map<Price, std::deque<std::pair<OrderId,Qty>>, std::greater<Price>> bids;
    std::map<Price, std::deque<std::pair<OrderId,Qty>>, std::less<Price>>    asks;
    std::unordered_map<OrderId, std::pair<Price, uint8_t>> index_;
    uint64_t fill_count = 0;

    template<typename Map>
    void sweep(Map& passive, OrderId id, Price limit, bool has_limit, Qty& qty) {
        while (qty > 0 && !passive.empty()) {
            auto it = passive.begin();
            if (has_limit && (passive.key_comp()(limit, it->first))) break;
            auto& q = it->second;
            while (qty > 0 && !q.empty()) {
                Qty f = std::min(qty, q.front().second);
                qty -= f; q.front().second -= f; ++fill_count;
                if (!q.front().second) { index_.erase(q.front().first); q.pop_front(); }
            }
            if (q.empty()) passive.erase(it);
        }
    }
    void add(OrderId id, Side side, Price px, Qty qty) {
        bool buy = side == Side::Buy;
        if (buy) sweep(asks, id, px, true, qty); else sweep(bids, id, px, true, qty);
        if (qty > 0) {
            if (buy) bids[px].push_back({id, qty}); else asks[px].push_back({id, qty});
            index_[id] = {px, uint8_t(buy ? 0u : 1u)};
        }
    }
    void cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return;
        auto [px, sd] = it->second;
        auto erase_from = [&](auto& map) {
            auto lit = map.find(px);
            if (lit == map.end()) return;
            auto& q = lit->second;
            q.erase(std::remove_if(q.begin(), q.end(), [id](auto& p){ return p.first == id; }), q.end());
            if (q.empty()) map.erase(lit);
        };
        if (sd == 0) erase_from(bids); else erase_from(asks);
        index_.erase(it);
    }
};

// bench/bench_replay.cpp — generate a synthetic trace and replay it,
// reporting a full latency histogram using LatencyHistogram.
//
// This is the kind of benchmark you'd run before and after a refactor
// to validate that latency characteristics haven't regressed.


// Generate a synthetic trace that mimics realistic order flow:
//   - Resting limit orders from market makers (60%)
//   - Aggressive limit/market orders from takers  (20%)
//   - Cancellations of resting orders             (12%)
//   - Modifications of resting orders' quantity    (8%)  -- previously absent
// across n_symbols independent instruments, with bursty inter-arrival
// timing (alternating quiet and burst periods, not a flat uniform rate) —
// a single uniform-rate, add/cancel-only, single-symbol stream was the
// most concrete gap in this benchmark: real feeds have quote updates, and
// real order flow is bursty (news, market open, an options strike getting
// hit) rather than a steady drip.
static void generate_trace(const char* path, int n_events, int n_symbols) {
    TraceWriter writer(path);
    std::mt19937_64 rng(0xDEADBEEF);

    // Simulated clock starting at "9:30 AM"
    uint64_t ts_ns  = 9ULL * 3600 * 1'000'000'000ULL;
    uint64_t id_gen = 1;

    // Track live order ids per symbol for cancel/modify targeting.
    std::vector<std::vector<OrderId>> live(static_cast<std::size_t>(n_symbols));
    for (auto& v : live) v.reserve(10000);

    // Burst state machine: quiet periods (500-2500ns/event, as the
    // original benchmark always used) alternating with burst periods
    // (20-100ns/event — roughly 10-50x denser), each lasting a randomised
    // number of events. This is deliberately simple (two states, not a
    // full point-process model) — the goal is "the rate visibly changes
    // over the course of the trace," which a flat uniform generator
    // cannot exercise at all, not a calibrated model of real market
    // microstructure.
    bool in_burst = true;
    int  events_until_flip = 200 + int(rng() % 800);

    auto rand_price = [&](Side s) -> Price {
        double mid    = 100.0;
        double spread = 0.02;
        double jitter = (rng() % 21 - 10) * 0.005;
        double base   = (s == Side::Buy) ? mid - spread / 2 : mid + spread / 2;
        return to_price(base + jitter);
    };

    for (int i = 0; i < n_events; ++i) {
        if (--events_until_flip <= 0) {
            in_burst        = !in_burst;
            events_until_flip = in_burst ? (200 + int(rng() % 800))
                                          : (5000 + int(rng() % 5000));
        }
        ts_ns += in_burst ? (20 + rng() % 80) : (500 + rng() % 2000);

        const SymbolId sym = SymbolId(rng() % uint64_t(n_symbols));
        auto& sym_live = live[sym];

        int roll = int(rng() % 100);
        MarketDataMsg msg{};
        msg.symbol = sym;

        if (roll < 60 || sym_live.empty()) {
            // New resting limit order.
            Side s = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            msg.msg_type   = MarketDataMsg::Type::NewOrder;
            msg.order_id   = id_gen++;
            msg.side       = s;
            msg.price      = rand_price(s);
            msg.qty        = 100 + (rng() % 10) * 100;
            msg.order_type = OrderType::Limit;
            writer.write(ts_ns, msg);
            sym_live.push_back(msg.order_id);

        } else if (roll < 80) {
            // Aggressive order (crosses the spread).
            Side s = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            msg.msg_type   = MarketDataMsg::Type::NewOrder;
            msg.order_id   = id_gen++;
            msg.side       = s;
            msg.price      = (s == Side::Buy) ? to_price(101.0) : to_price(99.0);
            msg.qty        = 100 + (rng() % 5) * 100;
            msg.order_type = (rng() % 3 == 0) ? OrderType::Market : OrderType::Limit;
            writer.write(ts_ns, msg);

        } else if (roll < 92) {
            // Cancel a random live order on this symbol.
            std::size_t idx = rng() % sym_live.size();
            msg.msg_type = MarketDataMsg::Type::CancelOrder;
            msg.order_id = sym_live[idx];
            writer.write(ts_ns, msg);
            sym_live[idx] = sym_live.back();
            sym_live.pop_back();

        } else {
            // Modify a random live order's quantity — previously this
            // benchmark generated no Modify traffic at all, despite
            // MatchingEngine fully supporting it (and despite it being
            // the message type most likely to expose priority-loss bugs
            // under real load, per the README's bug #4).
            std::size_t idx = rng() % sym_live.size();
            msg.msg_type = MarketDataMsg::Type::ModifyOrder;
            msg.order_id = sym_live[idx];
            msg.qty      = 100 + (rng() % 10) * 100;   // new quantity
            writer.write(ts_ns, msg);
            // The order stays live either way — modify doesn't remove it.
        }
    }

    printf("Generated trace: %lu events -> %s (%d symbols, bursty timing)\n",
           writer.events_written(), path, n_symbols);
}

int main(int argc, char** argv) {
    const int n_events  = (argc > 1) ? std::atoi(argv[1]) : 500'000;
    const int n_symbols = (argc > 2) ? std::atoi(argv[2]) : 4;
    const char* trace_path = "/tmp/engine_trace.bin";

    printf("=== Order Flow Replay Benchmark ===\n\n");

    // ── Generate trace ──────────────────────────────────────────────────────
    printf("Generating %d synthetic events across %d symbols...\n", n_events, n_symbols);
    generate_trace(trace_path, n_events, n_symbols);

    // ── Set up engine ────────────────────────────────────────────────────────
    MatchingEngine engine;
    engine.register_symbol(0);
    for (int s = 1; s < n_symbols; ++s) engine.register_symbol(SymbolId(s));
    engine.start();

    // ── Replay at max speed ──────────────────────────────────────────────────
    printf("\n[Mode: MaxSpeed]\n");
    {
        OrderFlowReplay::Config cfg;
        cfg.mode          = OrderFlowReplay::Mode::MaxSpeed;
        cfg.warmup_events = 10'000;
        cfg.verbose       = false;

        OrderFlowReplay replay(engine, cfg);
        ReplayResult result;
        replay.replay(trace_path, result);
        result.print();
    }

    // ── Replay at 1M msgs/sec (throttled) ───────────────────────────────────
    printf("\n[Mode: Throttled @ 1M msgs/sec]\n");
    {
        OrderFlowReplay::Config cfg;
        cfg.mode          = OrderFlowReplay::Mode::Throttled;
        cfg.throttle_mps  = 1'000'000;
        cfg.warmup_events = 5'000;

        OrderFlowReplay replay(engine, cfg);
        ReplayResult result;
        replay.replay(trace_path, result);
        result.print();
    }

    engine.stop();

    // ── Naive std::map baseline ───────────────────────────────────────────────
    printf("\n[Naive std::map reference — same trace, single thread]\n");
    {
        // One NaiveBook per symbol — NaiveBook itself has no symbol
        // dimension (it's a single-instrument reference), so with a
        // multi-symbol trace, a single shared instance would cross orders
        // from DIFFERENT symbols against each other, inflating the fill
        // count and misrepresenting the workload this is meant to
        // contextualise. Mirrors how the real engine (MatchingEngine)
        // keeps one independent OrderBook per registered symbol.
        std::vector<NaiveBook> books(static_cast<std::size_t>(n_symbols));
        using namespace engine;
        auto t0 = std::chrono::steady_clock::now();

        // Re-read the trace using the SAME on-disk format TraceWriter
        // actually wrote (TraceHeader + packed TraceEvent, from
        // core/replay.hpp) — a previous version of this section read
        // records as a raw `struct { uint64_t ts; MarketDataMsg msg; }`,
        // which does not match TraceEvent's packed, explicit-field layout
        // at all (different field order, different types, no packing).
        // That silently misinterpreted every record's bytes: msg_type
        // compared against nothing that ever matched NewOrder or
        // CancelOrder, so this whole baseline processed zero real events
        // regardless of the trace's actual content — confirmed directly,
        // it reported "Fills: 0" out of 500,000 events including a
        // documented 20% aggressive/crossing share, which is only
        // possible if the data being read is not the data that was
        // written.
        FILE* f = fopen(trace_path, "rb");
        if (f) {
            TraceHeader hdr{};
            size_t n = 0;
            if (fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == kTraceMagic) {
                TraceEvent ev{};
                while (fread(&ev, sizeof(ev), 1, f) == 1) {
                    MarketDataMsg msg = trace_event_to_msg(ev);
                    if (msg.symbol >= books.size()) { ++n; continue; }
                    NaiveBook& book = books[msg.symbol];
                    if (msg.msg_type == MarketDataMsg::Type::NewOrder) {
                        book.add(msg.order_id, msg.side, msg.price, msg.qty);
                    } else if (msg.msg_type == MarketDataMsg::Type::CancelOrder) {
                        book.cancel(msg.order_id);
                    }
                    // Modify isn't implemented in this reference baseline —
                    // NaiveBook is a throughput comparison point, not a
                    // second correctness model (that's test_conservation.cpp's
                    // job, against a model that DOES implement it). Skipping
                    // Modify here just means this baseline's fill count
                    // will differ from the engine's on a trace that includes
                    // modifies — expected, not a bug.
                    ++n;
                }
            } else {
                printf("  (trace header missing or bad magic — skipping naive baseline)\n");
            }
            fclose(f);
            double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            uint64_t total_fills = 0;
            for (auto& b : books) total_fills += b.fill_count;
            printf("  Events    : %zu\n", n);
            printf("  Fills     : %llu\n", (unsigned long long)total_fills);
            printf("  Throughput: %.2f M msg/sec\n", n / elapsed / 1e6);
            printf("  vs engine : see MaxSpeed row above\n");
        }
    }

    return 0;
}