// bench/bench_latency.cpp
//
// Measures per-event latency at two levels:
//
//   1. Order book (single-threaded, no IPC):
//      Isolates the matching engine core. No SPSC queue overhead.
//      This is the number that matters for the matching path itself.
//
//   2. End-to-end (submit → poll):
//      Includes SPSC queues and inter-thread cache coherence traffic.
//      Not every order produces a fill report; we measure submit latency
//      unconditionally and poll opportunistically.

#include "core/order_book.hpp"
#include "core/matching_engine.hpp"
#include <cstdio>
#include <vector>
#include <algorithm>
#include <numeric>
#include <time.h>

using namespace engine;

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
}

static void print_pcts(std::vector<uint64_t>& v, const char* label) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    const double mean = (double)std::accumulate(v.begin(), v.end(), 0ULL) / n;
    printf("  %-32s  n=%-7zu  mean=%5.0f ns  "
           "p50=%4lu  p90=%4lu  p99=%5lu  p99.9=%6lu ns\n",
           label, n, mean,
           v[n*50/100], v[n*90/100], v[n*99/100], v[n*999/1000]);
}

static void bench_order_book() {
    printf("── 1. Order Book (single-threaded, no SPSC) ───────────────────────\n\n");

    std::vector<ExecutionReport> fills;
    fills.reserve(1 << 20);
    auto sink = [&](const ExecutionReport& r){ fills.push_back(r); };
    OrderBook book(0, sink);

    uint64_t id = 1;
    auto make = [&](Side s, Price p, Qty q) {
        Order o{}; o.id=id++; o.price=p; o.qty=q; o.qty_remaining=q;
        o.symbol=0; o.side=s; o.type=OrderType::Limit; o.status=OrderStatus::New;
        return o;
    };

    for (int i = 0; i < 200; ++i) {
        book.add_order(make(Side::Sell, to_price(100.0 + i*0.01), 1000));
        book.add_order(make(Side::Buy,  to_price(99.0  - i*0.01), 1000));
    }

    // Warm up
    for (int i = 0; i < 50'000; ++i)
        book.add_order(make((i%2)?Side::Buy:Side::Sell, to_price(99.5), 10));

    static constexpr int N = 500'000;

    // Passive orders (no cross)
    std::vector<uint64_t> passive; passive.reserve(N);
    for (int i = 0; i < N; ++i) {
        Side s = (i%2==0) ? Side::Buy : Side::Sell;
        Price p = (s==Side::Buy) ? to_price(98.0) : to_price(102.0);
        auto o = make(s, p, 100);
        uint64_t t0 = now_ns(); book.add_order(o); passive.push_back(now_ns()-t0);
    }

    // Aggressive orders (cross, generate fills)
    std::vector<uint64_t> aggr; aggr.reserve(N);
    for (int i = 0; i < N; ++i) {
        Side s = (i%2==0) ? Side::Buy : Side::Sell;
        Price p = (s==Side::Buy) ? to_price(100.005) : to_price(99.995);
        auto o = make(s, p, 50);
        uint64_t t0 = now_ns(); book.add_order(o); aggr.push_back(now_ns()-t0);
    }

    // Cancel
    std::vector<OrderId> ids; ids.reserve(N);
    for (int i = 0; i < N; ++i) {
        Side s = (i%2==0) ? Side::Buy : Side::Sell;
        Order o = make(s, (s==Side::Buy)?to_price(97.0):to_price(103.0), 100);
        ids.push_back(o.id); book.add_order(o);
    }
    std::vector<uint64_t> cancel; cancel.reserve(N);
    for (OrderId oid : ids) {
        uint64_t t0 = now_ns(); book.cancel_order(oid); cancel.push_back(now_ns()-t0);
    }

    print_pcts(passive, "add_order (passive, no fill)");
    print_pcts(aggr,    "add_order (aggressive, fill)");
    print_pcts(cancel,  "cancel_order");

    uint64_t t0 = now_ns();
    for (int i = 0; i < 1'000'000; ++i)
        book.add_order(make((i%2)?Side::Buy:Side::Sell, to_price(99.5+(i%3)*0.01), 10));
    double s = (now_ns()-t0)/1e9;
    printf("\n  Throughput: %.1f M msg/sec  (1M orders / %.0f ms)\n", 1.0/s, s*1000.0);
    printf("  Fills: %zu\n\n", fills.size());
}

static void bench_end_to_end() {
    printf("── 2. End-to-End (SPSC → match → SPSC) ───────────────────────────\n\n");
    printf("  Measures SPSC push cost from the producer thread.\n");
    printf("  The matching engine runs asynchronously; true order-to-fill\n");
    printf("  latency = SPSC_push + match_latency (see section 1 above).\n\n");

    MatchingEngine eng;
    eng.register_symbol(0);
    eng.start();

    // Seed book
    static uint64_t sid = 1;
    for (int i = 0; i < 100; ++i) {
        for (auto [s, p] : {std::pair{Side::Sell, to_price(100.0+i*0.01)},
                            std::pair{Side::Buy,  to_price(99.0-i*0.01)}}) {
            MarketDataMsg m{}; m.msg_type=MarketDataMsg::Type::NewOrder;
            m.symbol=0; m.side=s; m.price=p; m.qty=1000;
            m.order_type=OrderType::Limit; m.order_id=sid++;
            while (!eng.submit(m)) __builtin_ia32_pause();
        }
    }
    // Let the engine drain the seed
    struct timespec ts{0, 20'000'000}; nanosleep(&ts, nullptr);

    // Warmup
    MarketDataMsg msg{}; msg.symbol=0; msg.order_type=OrderType::Limit;
    uint64_t id = 200'000;
    for (int i = 0; i < 20'000; ++i) {
        msg.order_id=++id; msg.side=(i%2==0)?Side::Buy:Side::Sell;
        msg.price=to_price(99.5); msg.qty=10;
        msg.msg_type=MarketDataMsg::Type::NewOrder;
        while (!eng.submit(msg)) __builtin_ia32_pause();
        ExecutionReport r; (void)eng.poll_report(r);   // warmup: drain, result irrelevant
    }

    static constexpr int N = 200'000;
    std::vector<uint64_t> lat; lat.reserve(N);
    uint64_t fills = 0;

    for (int i = 0; i < N; ++i) {
        msg.order_id=++id;
        msg.side=(i%2==0)?Side::Buy:Side::Sell;
        msg.price=(i%5<3) ?
            ((msg.side==Side::Buy)?to_price(98.0):to_price(102.0)) :
            ((msg.side==Side::Buy)?to_price(100.5):to_price(99.5));
        msg.qty=50+(i%5)*10; msg.msg_type=MarketDataMsg::Type::NewOrder;

        uint64_t t0=now_ns();
        while (!eng.submit(msg)) __builtin_ia32_pause();
        lat.push_back(now_ns()-t0);

        ExecutionReport r; if (eng.poll_report(r)) ++fills;
    }

    eng.stop();

    print_pcts(lat, "submit latency (all orders)");
    printf("\n  Fill reports observed: %lu / %d  (%.0f%%)\n\n",
           fills, N, 100.0*fills/N);
}

int main() {
    printf("=== Low-Latency Trading Engine — Latency Benchmark ===\n\n");
    printf("  Environment: shared container, no isolcpus, no SCHED_FIFO.\n");
    printf("  See docs/linux-tuning.md for production tuning (reduces p99 10-100x).\n\n");
    bench_order_book();
    bench_end_to_end();
    return 0;
}
