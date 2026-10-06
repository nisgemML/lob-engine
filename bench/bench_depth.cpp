// bench/bench_depth.cpp — add/cancel cost as the book gets deep.
//
// Every other benchmark here keeps the book shallow (a few hundred live
// orders), which is why none of them noticed that the order-id index
// degraded to a linear scan under sequential ids. This one rests N
// non-crossing orders with sequential ids, then cancels them all, for N up
// to 60,000 (kMaxOrders = 65,536), and reports ns/op plus the index's
// longest probe cluster. It also counts minor page faults during the run:
// the book prefaults itself at construction, so the expected count is 0.
//
// Usage: bench_depth [runs]   (median of runs per depth; default 5)
#include "core/order_book.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/resource.h>
#include <vector>
using namespace engine;
using clk = std::chrono::steady_clock;

static long minflt() { rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_minflt; }
static double med(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

int main(int argc, char** argv) {
    const int runs = argc > 1 ? std::max(1, std::atoi(argv[1])) : 5;
    std::printf("%8s %12s %12s %10s %8s\n", "live", "add ns/op", "cancel ns/op", "cluster", "faults");
    for (int n : {1'000, 4'000, 8'000, 16'000, 32'000, 60'000}) {
        std::vector<double> add, can; uint32_t cluster = 0; long faults = 0;
        for (int r = 0; r < runs; ++r) {
            auto noop = [](const ExecutionReport&) {};
            auto book = std::make_unique<OrderBook>(0, noop);   // prefaults here
            const long f0 = minflt();
            auto t0 = clk::now();
            for (int i = 0; i < n; ++i) {
                Order o{}; o.id = OrderId(1'000'000'000ULL + i); o.type = OrderType::Limit;
                const bool buy = i & 1; o.side = buy ? Side::Buy : Side::Sell;
                o.price = to_price((buy ? 90.0 : 105.0) + (i % 500) * 0.01);
                o.qty = o.qty_remaining = 10;
                if (!book->add_order(o)) { std::fprintf(stderr, "add failed at %d\n", i); return 1; }
            }
            auto t1 = clk::now();
            cluster = book->index_longest_cluster();
            auto t2 = clk::now();
            for (int i = 0; i < n; ++i) (void)book->cancel_order(OrderId(1'000'000'000ULL + i));
            auto t3 = clk::now();
            faults += minflt() - f0;
            add.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
            can.push_back(std::chrono::duration<double, std::nano>(t3 - t2).count() / n);
        }
        std::printf("%8d %12.1f %12.1f %10u %8ld\n", n, med(add), med(can), cluster, faults);
    }
}
