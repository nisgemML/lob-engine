// tests/test_multi_symbol_engine.cpp — Regression tests for MultiSymbolEngine.
//
// MultiSymbolEngine previously did not compile at all (queue.pop() and
// engine.on_message() don't exist — the real names are try_pop() and
// submit()), and was never caught because nothing in this codebase ever
// instantiated the class template and called start() on it — a class
// template's member function bodies are only fully checked when actually
// used. Confirmed directly: a one-file program instantiating
// MultiSymbolEngine<4> and calling register_symbol()/start() produced two
// hard compile errors at exactly those two call sites.
//
// A second bug, independent of the first: register_symbol() placed a
// symbol by hashing its ticker STRING; submit() picked a shard by masking
// the message's raw integer SymbolId directly — two unrelated numbers, so
// even with the method names fixed, messages would very likely route to a
// shard that never registered that symbol. Fixed by recording the shard
// chosen at registration time and routing by table lookup at submit time.
//
// A third, related discovery while building this test: MatchingEngine's
// SCHED_FIFO elevation was unconditional. Two busy-poll SCHED_FIFO threads
// at the same priority sharing one CPU split scheduled iterations roughly
// 8,800:1 in a direct measurement — not a deadlock, but severe enough that
// a MultiSymbolEngine with more shards than available cores would leave
// most shards almost never scheduled. These tests deliberately run with
// unpinned shards (ShardConfig::cpu_affinity = -1, the default), which
// now also skips SCHED_FIFO — see matching_engine.hpp's start() doc
// comment and docs/design.md §5 for the full reasoning. This is a
// correctness test suite, not a hardware benchmark; forcing SCHED_FIFO
// with no dedicated cores available would make these tests themselves
// flaky for the same reason a real deployment would be broken.

#include "core/multi_symbol_engine.hpp"
#include <cstdio>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <set>

using namespace engine;
using namespace std::chrono_literals;

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

static MarketDataMsg make_msg(MarketDataMsg::Type type, OrderId id, SymbolId sym,
                               Side side, Price price, Qty qty,
                               OrderType otype = OrderType::Limit) {
    MarketDataMsg m{};
    m.msg_type = type; m.order_id = id; m.symbol = sym;
    m.side = side; m.price = price; m.qty = qty; m.order_type = otype;
    return m;
}

// Poll every shard's reports for up to timeout_ms, dispatching to on_exec.
template <std::size_t N>
static std::size_t poll_until(MultiSymbolEngine<N>& mse, std::size_t want,
                               std::atomic<std::size_t>& got, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        mse.poll_reports();
        if (got.load() >= want) break;
        __builtin_ia32_pause();
    }
    return got.load();
}

// ── This is the compile-existence test itself ────────────────────────────────
// If MultiSymbolEngine's method-name bugs regress, this test file simply
// fails to build — which is exactly the signal that was missing before.

static void test_instantiates_and_starts() {
    printf("MultiSymbolEngine<4> instantiates, registers, starts, stops cleanly:\n");
    std::atomic<std::size_t> fills{0};
    MultiSymbolEngine<4> mse([&](const ExecutionReport&) { fills.fetch_add(1); });

    CHECK(mse.register_symbol("AAPL", 0), "register AAPL succeeds");
    CHECK(mse.register_symbol("MSFT", 1), "register MSFT succeeds");
    mse.start();
    mse.stop();
    CHECK(true, "start()/stop() completed without hanging or crashing");
}

// ── Correct routing: registration and submission must agree ────────────────

static void test_cross_generates_exactly_one_report() {
    printf("MultiSymbolEngine: a crossing pair on a registered symbol fills:\n");
    std::atomic<std::size_t> fills{0};
    MultiSymbolEngine<4> mse([&](const ExecutionReport&) { fills.fetch_add(1); });
    CHECK(mse.register_symbol("AAPL", 0), "register AAPL");
    mse.start();

    auto sell = make_msg(MarketDataMsg::Type::NewOrder, 1, 0, Side::Sell, to_price(100.0), 100);
    auto buy  = make_msg(MarketDataMsg::Type::NewOrder, 2, 0, Side::Buy,  to_price(100.0), 100);
    CHECK(mse.submit(sell), "submit resting sell");
    std::this_thread::sleep_for(5ms);
    CHECK(mse.submit(buy), "submit crossing buy");

    std::size_t seen = poll_until(mse, 1, fills);
    CHECK(seen == 1, "exactly one execution report dispatched for a simple 1-for-1 cross");

    mse.stop();
}

static void test_submit_to_unregistered_symbol_fails_cleanly() {
    printf("MultiSymbolEngine: submitting to an unregistered symbol is rejected:\n");
    MultiSymbolEngine<4> mse([](const ExecutionReport&) {});
    CHECK(mse.register_symbol("AAPL", 0), "register AAPL");
    mse.start();

    auto msg = make_msg(MarketDataMsg::Type::NewOrder, 1, 99, Side::Buy, to_price(1.0), 1);
    CHECK(!mse.submit(msg), "submit to symbol 99 (never registered) returns false, doesn't crash");

    mse.stop();
}

// ── Each symbol's orders always land on the shard it was registered to ──────
//
// This is the direct regression test for the string-hash-vs-raw-id routing
// mismatch: register several symbols, submit orders for each, and confirm
// every fill's messages were actually processed by the shard that symbol
// was assigned to at registration — not routed by a different, disagreeing
// computation at submit time.

static void test_routing_consistent_with_registration() {
    printf("MultiSymbolEngine: every symbol's traffic stays on its registered shard:\n");
    constexpr std::size_t N = 4;
    std::atomic<std::size_t> fills{0};
    MultiSymbolEngine<N> mse([&](const ExecutionReport&) { fills.fetch_add(1); });

    // Register enough symbols that, with 4 shards, at least two almost
    // certainly land on different shards (birthday-paradox-safe margin).
    const char* tickers[] = {"AAPL", "MSFT", "GOOG", "AMZN", "TSLA", "NVDA", "META", "NFLX"};
    constexpr std::size_t n_symbols = 8;
    for (SymbolId s = 0; s < n_symbols; ++s)
        CHECK(mse.register_symbol(tickers[s], s), "register succeeds");

    mse.start();

    OrderId next_id = 1;
    std::size_t expected_fills = 0;
    for (SymbolId s = 0; s < n_symbols; ++s) {
        auto sell = make_msg(MarketDataMsg::Type::NewOrder, next_id++, s, Side::Sell,
                              to_price(50.0 + s), 10);
        auto buy  = make_msg(MarketDataMsg::Type::NewOrder, next_id++, s, Side::Buy,
                              to_price(50.0 + s), 10);
        CHECK(mse.submit(sell), "submit sell for this symbol's shard");
        std::this_thread::sleep_for(2ms);
        CHECK(mse.submit(buy), "submit crossing buy for this symbol's shard");
        ++expected_fills;
    }

    std::size_t seen = poll_until(mse, expected_fills, fills, 3000);
    CHECK(seen == expected_fills,
          "every symbol's cross filled — a mismatch here means at least one "
          "symbol's orders were split across two different shards (the "
          "exact failure mode of the original string-hash-vs-raw-id bug)");

    // At least two distinct shards must actually have done work — if every
    // single symbol landed on the same one shard, this test wouldn't be
    // exercising cross-shard routing at all (it would still pass by
    // accident on the buggy code, since a single shard can't disagree with
    // itself about where a message belongs).
    auto st = mse.stats();
    int shards_with_work = 0;
    for (auto& s : st) if (s.msgs_processed > 0) ++shards_with_work;
    CHECK(shards_with_work >= 2,
          "symbols actually spread across multiple shards in this test "
          "(otherwise the routing-consistency check above is not meaningful)");

    mse.stop();
}

// ── Unpinned/no-SCHED_FIFO path is exercised by every test above already —
// this test specifically checks the reported state matches reality. ────────

static void test_unpinned_shards_report_honestly() {
    printf("MultiSymbolEngine: unpinned shards report pinned=false, realtime=false:\n");
    MultiSymbolEngine<2> mse([](const ExecutionReport&) {});
    CHECK(mse.register_symbol("AAPL", 0), "register AAPL");
    mse.start();
    std::this_thread::sleep_for(10ms);

    for (auto& s : mse.stats()) {
        CHECK(!s.pinned,   "shard reports pinned=false when cpu_affinity was never set");
        CHECK(!s.realtime, "shard reports realtime=false when unpinned — SCHED_FIFO must not "
                            "be silently applied without a dedicated core (see matching_engine.cpp)");
    }
    mse.stop();
}

int main() {
    printf("=== MultiSymbolEngine Tests ===\n\n");
    test_instantiates_and_starts();
    test_cross_generates_exactly_one_report();
    test_submit_to_unregistered_symbol_fails_cleanly();
    test_routing_consistent_with_registration();
    test_unpinned_shards_report_honestly();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
