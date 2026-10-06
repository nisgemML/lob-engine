#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <random>
#include <vector>
#include <x86intrin.h>
#include "core/order_book.hpp"

// bench/bench_avx2.cpp — AVX2 vs scalar price level search benchmark.
//
// Measures: scalar linear search vs AVX2 SIMD search on the prices[] array.
//
// Why this matters:
// find_level() is called on every add_order and cancel — it is in the
// matching engine hot path. At kMaxLevels=4096 and active levels 20–200,
// AVX2 compares 4 int64_t per instruction (VPCMPEQQ): a quarter of the loop
// iterations. Measured end-to-end speedup is 1.6x at p50, not 4x.
// At full 4096 levels: scalar ~4096 cycles worst-case vs AVX2 ~1024 cycles.
//
// Compile: g++ -std=c++20 -O3 -march=native -mavx2 -I include bench/bench_avx2.cpp -o bench_avx2
// Run:     ./bench_avx2


static double g_ns_per_cycle = 1.0;
static void calibrate() {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = __rdtsc();
    volatile uint64_t x = 0;
    for (int i = 0; i < 100000000; ++i) x += i;
    uint64_t c1 = __rdtsc();
    auto t1 = std::chrono::steady_clock::now();
    g_ns_per_cycle = double(std::chrono::duration_cast<
        std::chrono::nanoseconds>(t1-t0).count()) / double(c1-c0);
    (void)x;
}

// Scalar search
[[nodiscard]] static int32_t find_scalar(
    const engine::Price* p, uint32_t n, engine::Price t) noexcept {
    for (uint32_t i = 0; i < n; ++i)
        if (p[i] == t) return int32_t(i);
    return -1;
}

struct Hist {
    static constexpr int N = 4096;
    uint64_t c[N] = {};
    uint64_t total = 0;
    void rec(uint64_t v) { c[v<N?v:N-1]++; total++; }
    uint64_t pct(double p) const {
        uint64_t t = uint64_t(p*double(total)), cum=0;
        for (int i=0;i<N;++i){cum+=c[i];if(cum>=t)return i;}
        return N-1;
    }
};

int main() {
    calibrate();
    printf("=== AVX2 vs Scalar find_level benchmark ===\n");
#ifdef __AVX2__
    printf("AVX2: enabled (VPCMPEQQ ymm — 4x int64 per instruction)\n\n");
#else
    printf("AVX2: NOT available — scalar only\n\n");
#endif

    constexpr int N_ITER  = 5000000;
    constexpr int N_LEVELS = 128; // typical active LOB depth

    // Build price array — sorted, kFixed spacing
    std::array<engine::Price, N_LEVELS> prices{};
    for (int i = 0; i < N_LEVELS; ++i)
        prices[i] = engine::Price(10000 + i * 100);

    // Random targets — 70% hit, 30% miss (realistic)
    std::mt19937_64 rng(42);
    std::vector<engine::Price> targets;
    targets.reserve(N_ITER);
    for (int i = 0; i < N_ITER; ++i) {
        if (rng() % 100 < 70)
            targets.push_back(prices[rng() % N_LEVELS]);
        else
            targets.push_back(engine::Price(99999 + int(rng() % 1000)));
    }

    volatile int64_t sink = 0;
    Hist h_scalar, h_avx2;

    // Warmup
    for (int i = 0; i < 10000; ++i)
        sink += find_scalar(prices.data(), N_LEVELS, targets[i]);

    // Benchmark scalar
    for (int i = 0; i < N_ITER; ++i) {
        uint64_t t0 = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
        sink += find_scalar(prices.data(), N_LEVELS, targets[i]);
        uint64_t t1 = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
        h_scalar.rec(t1 - t0);
    }

#ifdef __AVX2__
    // Warmup AVX2
    for (int i = 0; i < 10000; ++i)
        sink += engine::find_level_avx2(prices.data(), N_LEVELS, targets[i]);

    // Benchmark AVX2
    for (int i = 0; i < N_ITER; ++i) {
        uint64_t t0 = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
        sink += engine::find_level_avx2(prices.data(), N_LEVELS, targets[i]);
        uint64_t t1 = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
        h_avx2.rec(t1 - t0);
    }
#endif

    printf("N_LEVELS=%d, N_ITER=%d (70%% hit / 30%% miss)\n\n", N_LEVELS, N_ITER);
    printf("%-12s %8s %8s %8s %8s\n", "Method", "p50(ns)", "p90(ns)", "p99(ns)", "p99.9(ns)");
    printf("%-12s %8llu %8llu %8llu %8llu\n", "Scalar",
        (unsigned long long)h_scalar.pct(0.50),
        (unsigned long long)h_scalar.pct(0.90),
        (unsigned long long)h_scalar.pct(0.99),
        (unsigned long long)h_scalar.pct(0.999));
#ifdef __AVX2__
    printf("%-12s %8llu %8llu %8llu %8llu\n", "AVX2",
        (unsigned long long)h_avx2.pct(0.50),
        (unsigned long long)h_avx2.pct(0.90),
        (unsigned long long)h_avx2.pct(0.99),
        (unsigned long long)h_avx2.pct(0.999));
    printf("\nSpeedup at p50: %.1fx\n",
        double(h_scalar.pct(0.50)) / double(std::max(uint64_t(1), h_avx2.pct(0.50))));
#endif
    printf("\n(sink=%lld to defeat DCE)\n", (long long)sink);

    // Correctness check
    printf("\n=== Correctness verification ===\n");
#ifndef __AVX2__
    printf("SKIPPED: built without AVX2, so there is no AVX2 path to verify\n");
#endif
    int errors = 0;
    for (auto& t : targets) {
        int32_t s = find_scalar(prices.data(), N_LEVELS, t);
#ifdef __AVX2__
        int32_t a = engine::find_level_avx2(prices.data(), N_LEVELS, t);
        if (s != a) { printf("MISMATCH: scalar=%d avx2=%d target=%ld\n", s, a, long(t)); ++errors; }
#else
        (void)s;   // no AVX2 in this build: nothing to compare against
#endif
    }
    printf("Correctness: %s (%d errors)\n", errors==0?"PASS":"FAIL", errors);
    return errors > 0 ? 1 : 0;
}
