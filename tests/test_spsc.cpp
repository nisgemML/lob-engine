// test_spsc.cpp — concurrent correctness tests for SPSCQueue.
//
// Tests run under ThreadSanitizer (-DTSAN=ON) to catch data races.
// The core correctness property: every item pushed by the producer
// is received exactly once by the consumer, in order.

#include "core/spsc_queue.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include <numeric>
#include <atomic>

using namespace engine;

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
            ++failed; \
        } else { \
            ++passed; \
        } \
    } while(0)

// ── Single-threaded correctness ───────────────────────────────────────────────


// Spin briefly, then yield. A pure PAUSE spin assumes the other thread is
// running on another core; on an oversubscribed host (CI runner, container,
// fewer cores than threads) each handoff would wait out a whole scheduler
// time slice, and this test previously did not finish in 150 s on one vCPU.
struct Backoff {
    int n = 0;
    void operator()() noexcept {
        if (n < 128) { __builtin_ia32_pause(); ++n; }
        else std::this_thread::yield();
    }
    void reset() noexcept { n = 0; }
};

static void test_basic_push_pop() {
    SPSCQueue<int, 16> q;
    CHECK(q.empty(), "Initially empty");

    CHECK(q.try_push(42), "Push succeeds");
    CHECK(!q.empty(), "Not empty after push");

    int v = 0;
    CHECK(q.try_pop(v), "Pop succeeds");
    CHECK(v == 42, "Pop returns correct value");
    CHECK(q.empty(), "Empty after pop");
}

static void test_capacity_boundary() {
    SPSCQueue<int, 8> q;  // capacity = 8, but ring uses 7 usable slots

    int popped = 0;
    // Fill to capacity.
    int pushes = 0;
    while (q.try_push(pushes)) ++pushes;
    CHECK(pushes > 0, "At least one push succeeded");

    // Now drain.
    int v;
    while (q.try_pop(v)) ++popped;
    CHECK(popped == pushes, "All pushed items received");
}

static void test_fifo_order() {
    SPSCQueue<int, 1024> q;
    for (int i = 0; i < 500; ++i)
        if (!q.try_push(i)) { fprintf(stderr, "FAIL: push %d into empty-enough queue\n", i); ++failed; return; }

    for (int i = 0; i < 500; ++i) {
        int v = -1;
        if (!q.try_pop(v) || v != i) {
            fprintf(stderr, "FAIL: FIFO order violated at i=%d got=%d\n", i, v);
            ++failed;
            return;
        }
    }
    ++passed;
}

// ── Concurrent stress test ────────────────────────────────────────────────────

static void test_concurrent_correctness() {
    static constexpr int kItems = 2'000'000;
    SPSCQueue<uint64_t, 1 << 17> q;

    std::atomic<bool> start{false};
    std::vector<uint64_t> received;
    received.reserve(kItems);

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (uint64_t i = 0; i < kItems; ++i) {
            { Backoff b; while (!q.try_push(i)) b(); }
        }
    });

    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        uint64_t v;
        Backoff b;
        while (static_cast<int>(received.size()) < kItems) {
            if (q.try_pop(v)) { received.push_back(v); b.reset(); }
            else b();
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    CHECK(static_cast<int>(received.size()) == kItems, "All items received");

    // Verify order and completeness.
    bool ordered = true;
    for (int i = 0; i < kItems; ++i) {
        if (received[i] != static_cast<uint64_t>(i)) { ordered = false; break; }
    }
    CHECK(ordered, "Items received in FIFO order");
}

// ── Wrap-around stress (power-of-two mask) ────────────────────────────────────

static void test_wrap_around() {
    SPSCQueue<uint32_t, 4> q;  // tiny queue — forces many wrap-arounds
    static constexpr int kRounds = 1'000'000;

    std::atomic<bool> go{false};
    uint64_t total_rx = 0;
    uint64_t order_errors = 0;

    std::thread prod([&] {
        while (!go.load()) std::this_thread::yield();
        for (uint32_t i = 0; i < kRounds; ++i) {
            { Backoff b; while (!q.try_push(i)) b(); }
        }
    });

    std::thread cons([&] {
        while (!go.load()) std::this_thread::yield();
        // Each value must be exactly the next one sent: a wrap-around bug
        // (lost, duplicated, or stale slot) shows up as a sequence break.
        // (This loop used to record `prev` and never check it.)
        uint32_t v = 0;
        int count = 0;
        Backoff b;
        while (count < kRounds) {
            if (q.try_pop(v)) {
                if (v != static_cast<uint32_t>(count)) ++order_errors;
                ++total_rx;
                ++count;
                b.reset();
            } else b();
        }
    });

    go.store(true, std::memory_order_release);
    prod.join();
    cons.join();

    CHECK(total_rx == kRounds, "Wrap-around: all items received");
    CHECK(order_errors == 0, "Wrap-around: every item in exact send order");
}

int main() {
    printf("=== SPSC Queue Tests ===\n\n");

    test_basic_push_pop();
    test_capacity_boundary();
    test_fifo_order();

    printf("Running concurrent correctness test (2M items)...\n");
    test_concurrent_correctness();

    printf("Running wrap-around stress test (1M rounds, queue depth=4)...\n");
    test_wrap_around();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
