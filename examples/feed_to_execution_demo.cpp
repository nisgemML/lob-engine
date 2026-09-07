// examples/feed_to_execution_demo.cpp — a real end-to-end pipeline:
// raw wire bytes -> MarketDataIngestion (feed handler) -> MatchingEngine
// -> ExecutionReport, all the way through, not each half tested in
// isolation.
//
// ── Why this exists ────────────────────────────────────────────────────────
//
// MarketDataIngestion (the wire-format feed handler) and MatchingEngine
// each had their own test coverage, but nothing anywhere in this codebase
// ever actually connected them: MarketDataIngestion writes decoded
// messages to its OWN SPSCQueue, and MatchingEngine reads from a
// DIFFERENT SPSCQueue it owns internally (only reachable via submit()).
// Wiring the two together — draining the ingestion queue and calling
// engine.submit() for each message — is exactly the "feed handler to
// matching engine" integration this demo builds and tests, because
// nothing else in this repository did.
//
// ── What this proves, concretely ──────────────────────────────────────────
//
//   1. A resting sell and a crossing buy, both encoded as raw wire bytes
//      (the same WireNewOrder struct a real feed would send), produce a
//      real ExecutionReport after going through decode -> gap check ->
//      submit -> match -> report — verified by content, not just "it
//      didn't crash."
//   2. A sequence gap in the wire stream is detected and reported via the
//      gap callback, end to end.
//   3. A malformed wire message (bad magic) is rejected at the ingestion
//      layer and never reaches the matching engine at all.
//   4. A Modify message changes a resting order's quantity, verified by
//      its effect on a subsequent match (the previously-larger quantity
//      is NOT what fills).

#include "core/market_data.hpp"
#include "core/matching_engine.hpp"
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <memory>
#include <atomic>

using namespace engine;
using namespace std::chrono_literals;

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); ++failed; } \
        else { ++passed; } \
    } while (0)

// ── Wire message builders — the same shapes a real feed would send ──────────

static std::vector<uint8_t> wire_new_order(uint64_t seq, uint64_t order_id, double price,
                                            uint32_t qty, uint16_t symbol, uint8_t side,
                                            uint8_t order_type = 0) {
    WireHeader hdr{};
    hdr.magic = kMagic; hdr.version = 1; hdr.msg_type = 1; hdr.seq_num = seq;
    WireNewOrder body{};
    body.order_id = order_id;
    body.price_fp = uint64_t(to_price(price));
    body.qty = qty; body.symbol_id = symbol; body.side = side; body.order_type = order_type;
    hdr.payload_len = sizeof(body);
    std::vector<uint8_t> buf(sizeof(hdr) + sizeof(body));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), &body, sizeof(body));
    return buf;
}

static std::vector<uint8_t> wire_modify(uint64_t seq, uint64_t order_id, uint32_t new_qty, uint16_t symbol) {
    WireHeader hdr{};
    hdr.magic = kMagic; hdr.version = 1; hdr.msg_type = 3; hdr.seq_num = seq;
    WireModifyOrder body{};
    body.order_id = order_id; body.new_qty = new_qty; body.symbol_id = symbol;
    hdr.payload_len = sizeof(body);
    std::vector<uint8_t> buf(sizeof(hdr) + sizeof(body));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), &body, sizeof(body));
    return buf;
}

// ── The glue this repo was missing: ingestion queue -> engine.submit() ──────
//
// A real deployment runs this as a loop on its own thread ("the pump").
// It is intentionally NOT part of MarketDataIngestion or MatchingEngine
// themselves — it's the integration point between two components that are
// each independently reusable without it (MarketDataIngestion could feed
// something other than a MatchingEngine; MatchingEngine can be fed
// synthetic MarketDataMsg structs directly, as every other test in this
// repo already does).
static std::size_t pump_ingestion_to_engine(MarketDataIngestion::OutboundQueue& q,
                                             MatchingEngine& engine) {
    std::size_t n = 0;
    MarketDataMsg msg;
    while (q.try_pop(msg)) {
        if (engine.submit(msg)) ++n;
    }
    return n;
}

static bool wait_for_report(MatchingEngine& engine, ExecutionReport& out, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.poll_report(out)) return true;
        __builtin_ia32_pause();
    }
    return false;
}

int main() {
    printf("=== Feed Handler -> Matching Engine -> Execution Report ===\n\n");

    // Heap-allocated, not stack locals: sizeof(MatchingEngine) is ~5.5MB
    // and sizeof(MarketDataIngestion::OutboundQueue) is ~2.5MB — both
    // large because this codebase's "no allocation on the hot path"
    // design puts fixed-size arrays directly in these objects, which is
    // exactly right for their INTENDED use as long-lived, heap-or-member
    // allocated objects, but the two together are EXACTLY the 8MB default
    // stack limit before adding a single other local variable. Confirmed
    // directly: as plain stack locals, this program segfaulted
    // immediately. This is the same hazard class as
    // tests/test_order_book.cpp's BookFixture fix (see that file's
    // comment) and is common enough with this codebase's core types that
    // it's called out explicitly in docs/design.md now — heap-allocate
    // MatchingEngine, MultiSymbolEngine, OrderBook, or
    // MarketDataIngestion::OutboundQueue via std::make_unique whenever
    // more than one might land in the same function.
    auto ingest_queue = std::make_unique<MarketDataIngestion::OutboundQueue>();
    MarketDataIngestion feed(*ingest_queue);

    int gap_events = 0;
    feed.on_gap([&](uint64_t expected, uint64_t got) {
        ++gap_events;
        printf("  [feed] sequence gap: expected %lu, got %lu\n", expected, got);
    });

    auto engine_ptr = std::make_unique<MatchingEngine>();
    MatchingEngine& engine = *engine_ptr;
    engine.register_symbol(0);
    engine.start();

    // ── 1. A resting sell + crossing buy, entirely as wire bytes ────────────
    printf("--- Test 1: wire bytes -> decode -> match -> execution report ---\n");
    auto sell = wire_new_order(1, /*order_id=*/100, /*price=*/100.00, /*qty=*/50, 0, /*side=*/1);
    auto buy  = wire_new_order(2, /*order_id=*/101, /*price=*/100.00, /*qty=*/50, 0, /*side=*/0);

    CHECK(feed.ingest(sell) == 1, "sell decodes from raw wire bytes");
    CHECK(pump_ingestion_to_engine(*ingest_queue, engine) == 1, "decoded sell reaches the engine's queue");
    std::this_thread::sleep_for(5ms);

    CHECK(feed.ingest(buy) == 1, "buy decodes from raw wire bytes");
    CHECK(pump_ingestion_to_engine(*ingest_queue, engine) == 1, "decoded buy reaches the engine's queue");

    ExecutionReport rpt{};
    bool got = wait_for_report(engine, rpt);
    CHECK(got, "a real execution report comes out the far end of the full pipeline");
    CHECK(rpt.exec_qty == 50, "fill qty matches what was wire-encoded");
    CHECK(rpt.exec_price == to_price(100.00), "fill price matches what was wire-encoded");
    printf("  Fill: qty=%u price=%.2f\n", rpt.exec_qty, from_price(rpt.exec_price));

    // ── 2. Sequence gap detection, end to end ──────────────────────────────
    printf("\n--- Test 2: sequence gap detected in the wire stream ---\n");
    auto skip_seq = wire_new_order(10, 102, 50.00, 10, 0, 1); // jumps from seq 2 to seq 10
    CHECK(feed.ingest(skip_seq) == 1, "message still decodes despite the gap");
    CHECK(gap_events == 1, "gap callback fired exactly once for the jump from seq 2 to seq 10");
    CHECK(feed.gaps_detected() == 1, "gaps_detected() counter agrees");
    // Deliberately NOT pumped to the engine: this test's only purpose is
    // proving gap detection fires at the ingestion layer, not exercising
    // matching. Draining and discarding it here (rather than pumping it
    // into engine.submit()) keeps it from resting in the book and
    // silently interfering with Test 4's price-time-priority expectations
    // below — exactly the kind of test cross-contamination this line
    // exists to prevent, found by this demo's own first run: an earlier
    // version of this file DID pump it through, leaving a resting sell at
    // $50 that had better price-time priority than Test 4's own order and
    // absorbed 10 shares of Test 4's fill before Test 4's own order ever
    // got a look — a real bug in this test's design, not in Modify or in
    // matching.
    MarketDataMsg discarded{};
    CHECK(ingest_queue->try_pop(discarded), "the gap message is drained, not left queued for a later pump to pick up");

    // ── 3. Malformed wire message never reaches the engine ─────────────────
    printf("\n--- Test 3: malformed wire message rejected before the engine ever sees it ---\n");
    auto bad = wire_new_order(11, 103, 1.00, 1, 0, 0);
    bad[0] ^= 0xFF; // corrupt the magic
    const uint64_t processed_before = engine.messages_processed();
    CHECK(feed.ingest(bad) == 0, "corrupted message fails to decode");
    CHECK(feed.parse_errors() == 1, "parse_errors() counts the rejection");
    CHECK(pump_ingestion_to_engine(*ingest_queue, engine) == 0, "nothing new reaches the engine's queue");
    std::this_thread::sleep_for(5ms);
    CHECK(engine.messages_processed() == processed_before,
          "the engine's own processed-message count is unaffected — "
          "the bad message never got anywhere near it");

    // ── 4. Modify changes what actually fills ──────────────────────────────
    printf("\n--- Test 4: wire-encoded Modify changes the resting quantity that fills ---\n");
    auto rest = wire_new_order(12, 200, 90.00, 500, 0, 1);   // rest 500 @ 90.00
    CHECK(feed.ingest(rest) == 1, "resting order decodes");
    (void)pump_ingestion_to_engine(*ingest_queue, engine);
    std::this_thread::sleep_for(5ms);

    auto modify = wire_modify(13, 200, /*new_qty=*/120, 0); // shrink to 120
    CHECK(feed.ingest(modify) == 1, "modify decodes");
    (void)pump_ingestion_to_engine(*ingest_queue, engine);
    std::this_thread::sleep_for(5ms);

    auto aggress = wire_new_order(14, 201, 90.00, 500, 0, 0); // try to buy 500 @ 90.00
    CHECK(feed.ingest(aggress) == 1, "aggressive buy decodes");
    (void)pump_ingestion_to_engine(*ingest_queue, engine);

    ExecutionReport rpt2{};
    bool got2 = wait_for_report(engine, rpt2);
    CHECK(got2, "the modified order still fills");
    CHECK(rpt2.exec_qty == 120,
          "fill qty is the MODIFIED 120, not the original 500 — the wire-encoded "
          "Modify genuinely changed matching behavior, not just decoded cleanly");
    printf("  Fill: qty=%u (expected 120, the post-modify quantity)\n", rpt2.exec_qty);

    engine.stop();

    printf("\n=== Summary ===\n");
    printf("Ingested   : %lu messages\n", feed.messages_ingested());
    printf("Gaps       : %lu\n", feed.gaps_detected());
    printf("Parse errs : %lu\n", feed.parse_errors());
    printf("Matches    : %lu\n", engine.matches_generated());

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
