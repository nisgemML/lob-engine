// tests/test_mpmc.cpp — correctness and TSan stress tests for MPMCQueue.
//
// Property being tested: every item pushed by any producer is received
// exactly once by some consumer.  We verify this by pushing unique values
// and checking the set of received values.

#include <memory>
#include "core/mpmc_queue.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <numeric>

using namespace engine;

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); ++failed; } \
        else { ++passed; } \
    } while(0)

// ── Single-threaded sanity ────────────────────────────────────────────────────

static void test_basic() {
    MPMCQueue<int, 16> q;
    CHECK(q.empty(), "Initially empty");
    CHECK(q.try_push(7), "Push succeeds");
    CHECK(!q.empty(), "Not empty after push");
    int v = 0;
    CHECK(q.try_pop(v), "Pop succeeds");
    CHECK(v == 7, "Correct value");
    CHECK(q.empty(), "Empty after pop");
}

static void test_fifo_single_thread() {
    MPMCQueue<int, 512> q;
    for (int i = 0; i < 256; ++i)
        if (!q.try_push(i)) { fprintf(stderr, "FAIL: push %d into empty-enough queue\n", i); ++failed; return; }
    for (int i = 0; i < 256; ++i) {
        int v = -1;
        if (!q.try_pop(v) || v != i) { fprintf(stderr, "FAIL: FIFO order at i=%d got=%d\n", i, v); ++failed; return; }
    }
    ++passed;
}

// ── N producers × M consumers ─────────────────────────────────────────────────

static void test_npmc(int n_producers, int n_consumers, int items_per_producer) {
    const int total = n_producers * items_per_producer;

    auto q_ptr = std::make_unique<MPMCQueue<uint64_t, 1 << 17>>(); auto& q = *q_ptr;
    std::atomic<bool> go{false};
    std::atomic<int>  producers_done{0};

    // Received items — one vector per consumer, merged after.
    std::vector<std::vector<uint64_t>> received(n_consumers);

    std::vector<std::thread> threads;

    // Producers: each pushes a unique range of values.
    for (int p = 0; p < n_producers; ++p) {
        threads.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) {}
            const uint64_t base = static_cast<uint64_t>(p) * items_per_producer;
            for (int i = 0; i < items_per_producer; ++i) {
                while (!q.try_push(base + i)) __builtin_ia32_pause();
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    // Consumers: drain until all producers are done and queue is empty.
    for (int c = 0; c < n_consumers; ++c) {
        threads.emplace_back([&, c] {
            while (!go.load(std::memory_order_acquire)) {}
            uint64_t v;
            while (true) {
                if (q.try_pop(v)) {
                    received[c].push_back(v);
                } else {
                    if (producers_done.load(std::memory_order_acquire) == n_producers
                        && q.empty())
                        break;
                    __builtin_ia32_pause();
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    // Merge all received vectors and verify completeness + no duplicates.
    std::vector<uint64_t> all;
    all.reserve(total);
    for (auto& rv : received)
        all.insert(all.end(), rv.begin(), rv.end());
    std::sort(all.begin(), all.end());

    CHECK(static_cast<int>(all.size()) == total, "All items received");

    bool no_dups = true;
    for (int i = 0; i + 1 < static_cast<int>(all.size()); ++i) {
        if (all[i] == all[i+1]) { no_dups = false; break; }
    }
    CHECK(no_dups, "No duplicate items");

    bool complete = true;
    for (int i = 0; i < total; ++i) {
        if (i >= static_cast<int>(all.size()) || all[i] != static_cast<uint64_t>(i)) {
            complete = false; break;
        }
    }
    CHECK(complete, "All expected values present");
}

// ── Full-queue back-pressure ───────────────────────────────────────────────────

static void test_backpressure() {
    MPMCQueue<int, 8> q;
    int pushes = 0;
    while (q.try_push(pushes)) ++pushes;
    CHECK(pushes > 0 && pushes <= 8, "Queue fills correctly");

    int pops = 0, v;
    while (q.try_pop(v)) ++pops;
    CHECK(pops == pushes, "All pushed items drained");
}

int main() {
    printf("=== MPMC Queue Tests ===\n\n");

    test_basic();
    test_fifo_single_thread();
    test_backpressure();

    printf("Running 1P×1C (200K items)...\n");
    test_npmc(1, 1, 200'000);

    printf("Running 2P×1C (100K items each)...\n");
    test_npmc(2, 1, 100'000);

    printf("Running 1P×2C (200K items)...\n");
    test_npmc(1, 2, 200'000);

    printf("Running 4P×4C (50K items each)...\n");
    test_npmc(4, 4, 50'000);

    printf("Running 8P×4C (25K items each)...\n");
    test_npmc(8, 4, 25'000);

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
