// test_spsc_unit.cpp — Fast single-threaded SPSC correctness tests.
// Completes in <1ms. Included in ctest.
// Concurrent stress tests: run test_spsc_stress manually (~30s).

#include "core/spsc_queue.hpp"
#include <cstdio>

using namespace engine;

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); ++failed; } \
         else { ++passed; } } while(0)

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
    SPSCQueue<int, 8> q;
    int pushes = 0;
    while (q.try_push(pushes)) ++pushes;
    CHECK(pushes > 0, "At least one push succeeded");
    int popped = 0, v;
    while (q.try_pop(v)) ++popped;
    CHECK(popped == pushes, "All pushed items drained");
}

static void test_fifo_order() {
    SPSCQueue<int, 1024> q;
    for (int i = 0; i < 500; ++i)
        if (!q.try_push(i)) { fprintf(stderr, "FAIL: push %d into empty-enough queue\n", i); ++failed; return; }
    for (int i = 0; i < 500; ++i) {
        int v = -1;
        if (!q.try_pop(v) || v != i) {
            fprintf(stderr, "FAIL: FIFO order violated at i=%d got=%d\n", i, v);
            ++failed; return;
        }
    }
    ++passed;
}

static void test_full_and_empty_returns() {
    SPSCQueue<int, 4> q;
    int count = 0;
    while (q.try_push(count)) ++count;
    CHECK(!q.try_push(999), "Push on full returns false");
    int dummy;
    while (q.try_pop(dummy)) {}
    CHECK(!q.try_pop(dummy), "Pop on empty returns false");
}

static void test_interleaved() {
    SPSCQueue<int, 8> q;
    int sent = 0, received = 0;
    // Every pop must return the next value in order, and every push into a
    // not-full queue must succeed. (This used to ignore both results and
    // count a pop as received whether or not it returned anything.)
    bool ok = true;
    int v = -1;
    for (int i = 0; i < 3; ++i) ok &= q.try_push(sent++);
    for (int i = 0; i < 2; ++i) { ok &= q.try_pop(v) && v == received; received++; }
    for (int i = 0; i < 3; ++i) ok &= q.try_push(sent++);
    while (q.try_pop(v)) { ok &= (v == received); received++; }
    CHECK(ok, "Interleaved pushes succeed and pops return values in order");
    CHECK(received == sent, "All interleaved items received");
}

int main() {
    printf("=== SPSC Queue Unit Tests ===\n\n");
    test_basic_push_pop();
    test_capacity_boundary();
    test_fifo_order();
    test_full_and_empty_returns();
    test_interleaved();
    printf("\nResults: %d passed, %d failed\n", passed, failed);
    printf("(Stress tests: run ./test_spsc_stress separately, ~30s)\n");
    return failed > 0 ? 1 : 0;
}
