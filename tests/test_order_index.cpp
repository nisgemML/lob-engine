// tests/test_order_index.cpp — the order-id index under deep books.
//
// The conservation test checks semantics against a reference model, but its
// random flow keeps the book shallow, so it never stressed the index. This
// file does:
//   1. 60,000 live orders with SEQUENTIAL ids (how exchanges assign them),
//      cancelled in random order. Under the old hash ((id*2654435761)>>32,
//      ~0.618*id) this took hours: ~12,500 probes per lookup, O(cluster^2)
//      deletes. The check is structural — the longest occupied run in the
//      index — not a timing threshold, which the old hash alone could pass.
//   2. Ids whose home slot is in the last 64 slots of the table, so probe
//      clusters wrap past the end — the backward-shift delete's cyclic
//      "does this entry stay?" test is exercised in both branches.
//   3. Every cancel succeeds exactly once; a repeated cancel fails; the
//      book ends empty.
#include "core/order_book.hpp"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>
using namespace engine;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", m); } } while (0)

static Order resting(OrderId id, bool buy, int tick) {
    Order o{}; o.id = id; o.type = OrderType::Limit;
    o.side = buy ? Side::Buy : Side::Sell;
    o.price = to_price((buy ? 90.0 : 105.0) + (tick % 500) * 0.01);   // never crosses
    o.qty = o.qty_remaining = 10;
    return o;
}

static void cancel_all_check(OrderBook& book, std::vector<OrderId> ids, uint64_t seed, const char* tag) {
    std::mt19937_64 rng(seed);
    std::shuffle(ids.begin(), ids.end(), rng);
    int ok = 0, again = 0;
    for (OrderId id : ids) {
        ok += book.cancel_order(id);
        again += book.cancel_order(id);           // must already be gone
    }
    char m1[96], m2[96];
    std::snprintf(m1, sizeof m1, "%s: every cancel succeeds once", tag);
    std::snprintf(m2, sizeof m2, "%s: no id cancellable twice", tag);
    CHECK(ok == int(ids.size()), m1);
    CHECK(again == 0, m2);
}

static void test_deep_sequential() {
    auto noop = [](const ExecutionReport&) {};
    auto book = std::make_unique<OrderBook>(0, noop);
    std::vector<OrderId> ids;
    for (int i = 0; i < 60'000; ++i) {
        const OrderId id = OrderId(1'000'000'000ULL + i);
        if (book->add_order(resting(id, i & 1, i))) ids.push_back(id);
    }
    CHECK(ids.size() == 60'000, "deep: 60,000 sequential ids all rest");
    const uint32_t lc = book->index_longest_cluster();
    std::printf("deep book, 60,000 sequential ids: longest index cluster = %u slots\n", lc);
    // Old hash: ~37,000 (most of the table in one run). Fixed: tens.
    CHECK(lc < 256, "deep: index clusters stay short under sequential ids");
    // a partial cancel/re-add wave in the middle of the cluster
    for (int i = 0; i < 60'000; i += 3) (void)book->cancel_order(ids[i]);
    for (int i = 0; i < 60'000; i += 3) CHECK(book->add_order(resting(ids[i], i & 1, i)), "deep: re-add after cancel");
    cancel_all_check(*book, ids, 42, "deep");
}

// Same formula as OrderIndex::home(): top 17 bits of id * 2^64/phi.
static uint32_t home_of(OrderId id) { return uint32_t((id * 0x9E3779B97F4A7C15ULL) >> (64 - 17)); }

static void test_wraparound_clusters() {
    std::vector<OrderId> ids;
    for (OrderId id = 1; ids.size() < 400; ++id)
        if (home_of(id) >= 131072u - 64u) ids.push_back(id);       // homes in the last 64 slots
    for (uint64_t seed : {1u, 2u, 3u, 4u, 5u}) {
        auto noop = [](const ExecutionReport&) {};
        auto book = std::make_unique<OrderBook>(0, noop);
        int rested = 0;
        for (size_t i = 0; i < ids.size(); ++i) rested += book->add_order(resting(ids[i], i & 1, int(i)));
        CHECK(rested == int(ids.size()), "wrap: 400 colliding ids all rest (cluster wraps past table end)");
        cancel_all_check(*book, ids, seed, "wrap");
    }
}

int main() {
    test_deep_sequential();
    test_wraparound_clusters();
    std::printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
