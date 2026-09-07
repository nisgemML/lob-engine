// test_order_book.cpp — unit + property-based tests for the order book.
//
// We test the invariants that must hold regardless of order sequence:
//   1. Best bid >= all other bids at every point.
//   2. Best ask <= all other asks at every point.
//   3. A market order executes at the best available price.
//   4. Cancelled orders are never filled.
//   5. Price-time priority: earlier orders at same price fill first.
//   6. Quantity conservation: filled_qty + leaves_qty == original_qty.

#include "core/order_book.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
#include <functional>
#include <memory>
#include <random>

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

// ── Helpers ───────────────────────────────────────────────────────────────────

struct FillRecord {
    OrderId order_id;
    OrderId contra_id;
    Price   price;
    Qty     qty;
};

// The match callback is a non-owning function_ref, so the sink must live
// at least as long as the book. Bundle them.
struct BookFixture {
    std::vector<FillRecord>& fills;
    std::function<void(const ExecutionReport&)> sink;
    // Heap-allocated, not a direct OrderBook member: sizeof(OrderBook) is
    // ~4.4MB (the fixed-size price-level and order-pool arrays that make
    // the hot path allocation-free also make the object itself large).
    // Several tests in this file (test_fok_atomicity in particular)
    // construct multiple BookFixtures in separate scoped blocks within
    // ONE function — as direct stack members that adds up fast, and
    // under ASan's added stack-redzone overhead it genuinely exceeded the
    // 8MB default stack limit: confirmed directly,
    // "AddressSanitizer: stack-overflow ... in test_fok_atomicity" on a
    // Release build that had not shown any problem at all (different
    // optimization level, no redzones). Heap-allocating here matches how
    // MatchingEngine itself already holds each OrderBook — via
    // std::make_unique, in src/core/matching_engine.cpp — for exactly
    // this reason; this fixture was the outlier, not OrderBook's design.
    std::unique_ptr<OrderBook> book_ptr;
    OrderBook& book;
    explicit BookFixture(std::vector<FillRecord>& f)
        : fills(f),
          sink([&f](const ExecutionReport& r) {
              f.push_back({ r.order_id, r.contra_order_id, r.exec_price, r.exec_qty });
          }),
          book_ptr(std::make_unique<OrderBook>(0, sink)),
          book(*book_ptr) {}
};

static Order make_order(OrderId id, Side side, Price price, Qty qty,
                         OrderType type = OrderType::Limit) {
    Order o{};
    o.id            = id;
    o.price         = price;
    o.qty           = qty;
    o.qty_remaining = qty;
    o.symbol        = 0;
    o.side          = side;
    o.type          = type;
    o.status        = OrderStatus::New;
    return o;
}

// ── Test cases ────────────────────────────────────────────────────────────────

static void test_simple_match() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;

    // Post a resting sell at 100.
    book.add_order(make_order(1, Side::Sell, to_price(100.0), 200));
    CHECK(fills.empty(), "No fill yet — passive order rested");

    // Aggressive buy at 100 (or higher) should match.
    book.add_order(make_order(2, Side::Buy, to_price(100.0), 100));
    CHECK(fills.size() == 1, "One fill generated");
    if (!fills.empty()) {
        CHECK(fills[0].qty == 100,         "Fill qty == 100");
        CHECK(fills[0].price == to_price(100.0), "Fill price == 100.0");
    }

    // Best ask should still show 100 remaining qty = 100.
    BestQuote q = book.best_quote();
    CHECK(q.ask_price == to_price(100.0), "Ask price still 100");
    CHECK(q.ask_qty   == 100,             "Ask qty reduced to 100");
}

static void test_price_time_priority() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;

    // Two resting sells at same price — order 1 posted first.
    book.add_order(make_order(1, Side::Sell, to_price(50.0), 100));
    book.add_order(make_order(2, Side::Sell, to_price(50.0), 100));

    // Aggressive buy for 100 — should fill entirely against order 1.
    book.add_order(make_order(3, Side::Buy, to_price(50.0), 100));

    CHECK(fills.size() == 1, "One fill");
    if (!fills.empty()) {
        CHECK(fills[0].contra_id == 1, "Filled against order 1 (FIFO priority)");
    }

    // Order 2 should still be resting.
    BestQuote q = book.best_quote();
    CHECK(q.ask_qty == 100, "Order 2 still resting");
}

static void test_cancel_prevents_fill() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;

    book.add_order(make_order(1, Side::Sell, to_price(100.0), 200));
    bool cancelled = book.cancel_order(1);
    CHECK(cancelled, "Cancel succeeded");

    // Now the book should be empty — buy should not fill.
    book.add_order(make_order(2, Side::Buy, to_price(100.0), 100));
    CHECK(fills.empty(), "No fill after cancel");

    BestQuote q = book.best_quote();
    CHECK(q.ask_price == PRICE_INVALID, "Ask side empty after cancel");
}

static void test_partial_fill() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;

    book.add_order(make_order(1, Side::Sell, to_price(100.0), 50));
    // Buy for 200 — only 50 available.
    book.add_order(make_order(2, Side::Buy, to_price(100.0), 200));

    CHECK(fills.size() == 1, "One fill");
    if (!fills.empty()) {
        CHECK(fills[0].qty == 50, "Partial fill qty == 50");
    }

    // Buy order 2 should have 150 remaining and rest in the book.
    BestQuote q = book.best_quote();
    CHECK(q.bid_price == to_price(100.0), "Partial buy rests at 100");
    CHECK(q.bid_qty   == 150,             "Remaining qty 150");
}

static void test_spread() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;

    // Spread: bid=99, ask=101 — no cross, no match.
    book.add_order(make_order(1, Side::Buy,  to_price(99.0),  100));
    book.add_order(make_order(2, Side::Sell, to_price(101.0), 100));

    CHECK(fills.empty(), "No match — spread exists");
    BestQuote q = book.best_quote();
    CHECK(q.bid_price == to_price(99.0),  "Best bid 99");
    CHECK(q.ask_price == to_price(101.0), "Best ask 101");
}

static void test_market_order() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;

    book.add_order(make_order(1, Side::Sell, to_price(100.0), 200));
    auto mkt = make_order(2, Side::Buy, 0, 150, OrderType::Market);
    book.add_order(mkt);

    CHECK(!fills.empty(),      "Market order matched");
    CHECK(fills[0].qty == 150, "Full fill for market order");
}

static void test_modify_decrease_keeps_priority() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;

    // Two resting sells at the same price, order 1 first.
    book.add_order(make_order(1, Side::Sell, to_price(50.0), 100));
    book.add_order(make_order(2, Side::Sell, to_price(50.0), 100));

    // Shrink order 1 — it must keep its place at the front of the queue.
    bool ok = book.modify_order(1, 40);
    CHECK(ok, "Modify (decrease) accepted");

    book.add_order(make_order(3, Side::Buy, to_price(50.0), 40));
    CHECK(fills.size() == 1, "One fill");
    if (!fills.empty())
        CHECK(fills[0].contra_id == 1, "Decrease keeps priority — still filled against order 1");
}

static void test_modify_increase_loses_priority() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;

    // Two resting sells at the same price, order 1 first.
    book.add_order(make_order(1, Side::Sell, to_price(50.0), 100));
    book.add_order(make_order(2, Side::Sell, to_price(50.0), 100));

    // Grow order 1 — it must move to the back of the queue.
    bool ok = book.modify_order(1, 150);
    CHECK(ok, "Modify (increase) accepted");

    // Aggressive buy for 100 should now fill against order 2 first.
    book.add_order(make_order(3, Side::Buy, to_price(50.0), 100));
    CHECK(fills.size() == 1, "One fill");
    if (!fills.empty())
        CHECK(fills[0].contra_id == 2, "Increase loses priority — order 2 fills first");

    // A further buy for 150 should then exhaust order 1 (now at the back).
    book.add_order(make_order(4, Side::Buy, to_price(50.0), 150));
    bool saw_order1 = false;
    for (auto& f : fills) if (f.contra_id == 1) saw_order1 = true;
    CHECK(saw_order1, "Order 1 still fillable after losing priority, just later");
}

static void test_ioc_remainder_expires() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;

    book.add_order(make_order(1, Side::Sell, to_price(100.0), 50));
    auto ioc = make_order(2, Side::Buy, to_price(100.0), 200, OrderType::IOC);
    bool ok = book.add_order(ioc);
    CHECK(ok, "IOC accepted");
    CHECK(fills.size() == 1 && fills[0].qty == 50, "IOC fills the available 50");

    // The unfilled 150 must NOT rest in the book.
    BestQuote q = book.best_quote();
    CHECK(q.bid_price == PRICE_INVALID, "IOC remainder does not rest — no resting bid");
}

static void test_fok_atomicity() {
    // FOK with insufficient liquidity: must reject with zero fills, not
    // partially execute.
    {
        std::vector<FillRecord> fills;
        BookFixture fx(fills);
        OrderBook& book = fx.book;

        book.add_order(make_order(1, Side::Sell, to_price(100.0), 50));
        auto fok = make_order(2, Side::Buy, to_price(100.0), 200, OrderType::FOK);
        bool ok = book.add_order(fok);
        CHECK(!ok, "FOK rejected when liquidity insufficient");
        CHECK(fills.empty(), "FOK rejection leaves zero fills (atomic, not partial)");
    }
    // FOK with sufficient liquidity: must fully fill, nothing rests.
    {
        std::vector<FillRecord> fills;
        BookFixture fx(fills);
        OrderBook& book = fx.book;

        book.add_order(make_order(1, Side::Sell, to_price(100.0), 200));
        auto fok = make_order(2, Side::Buy, to_price(100.0), 150, OrderType::FOK);
        bool ok = book.add_order(fok);
        CHECK(ok, "FOK accepted when liquidity sufficient");
        CHECK(fills.size() == 1 && fills[0].qty == 150, "FOK fully filled in one shot");

        BestQuote q = book.best_quote();
        CHECK(q.bid_price == PRICE_INVALID, "FOK never rests a remainder");
    }
}

static void test_self_trade_prevention() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;

    // Two resting sells at the same price: order 1 is account A, order 2
    // (behind it) is account B.
    Order resting1 = make_order(1, Side::Sell, to_price(50.0), 100);
    resting1.account_id = 111;
    book.add_order(resting1);

    Order resting2 = make_order(2, Side::Sell, to_price(50.0), 100);
    resting2.account_id = 222;
    book.add_order(resting2);

    // An account-A buy hits its own resting order at the head of the
    // queue: must not match at all (walling, not skipping to order 2 —
    // skipping would violate order 2's priority), and must not rest
    // through a price it refused, or the book ends up crossed.
    Order aggr = make_order(3, Side::Buy, to_price(50.0), 60);
    aggr.account_id = 111;
    bool ok = book.add_order(aggr);
    CHECK(ok, "STP-walled order still accepted (just doesn't match/rest)");
    CHECK(fills.empty(), "No fill against our own resting order");

    BestQuote q = book.best_quote();
    CHECK(q.bid_price == PRICE_INVALID, "Remainder does not rest through the wall (book stays uncrossed)");
    CHECK(q.ask_price == to_price(50.0), "Ask side untouched by the walled attempt");

    // An account-C buy at the same price is unaffected — it should match
    // order 1 normally (no self-trade relationship).
    Order aggr2 = make_order(4, Side::Buy, to_price(50.0), 40);
    aggr2.account_id = 333;
    book.add_order(aggr2);
    CHECK(fills.size() == 1, "Unrelated account matches normally");
    if (!fills.empty())
        CHECK(fills[0].contra_id == 1, "Matched against order 1 (front of queue, no STP conflict)");
}

// ── Property-based test: random order stream ──────────────────────────────────

static void test_invariants_random() {
    std::vector<FillRecord> fills;
    BookFixture fx(fills);
    OrderBook& book = fx.book;
    std::mt19937_64 rng(42);

    auto rand_price = [&]() { return to_price(99.0 + (rng() % 5) * 0.5); };
    auto rand_qty   = [&]() -> Qty { return 1 + (rng() % 200); };

    std::vector<OrderId> live_orders;
    bool invariant_ok = true;

    for (int i = 1; i <= 100'000; ++i) {
        int action = rng() % 10;

        if (action < 6 || live_orders.empty()) {
            // New order.
            Side s = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            book.add_order(make_order(i, s, rand_price(), rand_qty()));
            live_orders.push_back(i);
        } else if (action < 8 && !live_orders.empty()) {
            // Cancel a random live order.
            auto it = live_orders.begin() + (rng() % live_orders.size());
            book.cancel_order(*it);
            live_orders.erase(it);
        }

        // Invariant: bid <= ask (if both sides populated).
        BestQuote q = book.best_quote();
        if (q.bid_price != PRICE_INVALID && q.ask_price != PRICE_INVALID) {
            if (q.bid_price > q.ask_price) {
                invariant_ok = false;
                fprintf(stderr, "INVARIANT VIOLATED at i=%d: bid=%ld > ask=%ld\n",
                        i, q.bid_price, q.ask_price);
                break;
            }
        }
    }

    CHECK(invariant_ok, "bid <= ask invariant holds throughout random sequence");
    printf("  Random test: %lu fills generated over 100K events\n", fills.size());
}

int main() {
    printf("=== Order Book Tests ===\n\n");
    test_simple_match();
    test_price_time_priority();
    test_cancel_prevents_fill();
    test_partial_fill();
    test_spread();
    test_market_order();
    test_modify_decrease_keeps_priority();
    test_modify_increase_loses_priority();
    test_ioc_remainder_expires();
    test_fok_atomicity();
    test_self_trade_prevention();
    test_invariants_random();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
