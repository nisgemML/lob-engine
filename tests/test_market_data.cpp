// tests/test_market_data.cpp — Tests for MarketDataIngestion
// (include/core/market_data.hpp), previously untested outside the
// libFuzzer harness (which checks crash-safety, not decode correctness).

#include "core/market_data.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace engine;

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); ++failed; } \
        else { ++passed; } \
    } while (0)

// ── Wire message builders ─────────────────────────────────────────────────────

static std::vector<uint8_t> make_new_order(uint64_t seq, uint64_t order_id,
                                            uint64_t price_fp, uint32_t qty,
                                            uint16_t symbol_id, uint8_t side,
                                            uint8_t order_type) {
    WireHeader hdr{};
    hdr.magic = kMagic; hdr.version = 1; hdr.msg_type = 1; hdr.seq_num = seq;
    WireNewOrder body{};
    body.order_id = order_id; body.price_fp = price_fp; body.qty = qty;
    body.symbol_id = symbol_id; body.side = side; body.order_type = order_type;
    hdr.payload_len = sizeof(body);

    std::vector<uint8_t> buf(sizeof(hdr) + sizeof(body));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), &body, sizeof(body));
    return buf;
}

static std::vector<uint8_t> make_cancel(uint64_t seq, uint64_t order_id, uint16_t symbol_id) {
    WireHeader hdr{};
    hdr.magic = kMagic; hdr.version = 1; hdr.msg_type = 2; hdr.seq_num = seq;
    WireCancelOrder body{};
    body.order_id = order_id; body.symbol_id = symbol_id;
    hdr.payload_len = sizeof(body);

    std::vector<uint8_t> buf(sizeof(hdr) + sizeof(body));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), &body, sizeof(body));
    return buf;
}

static std::vector<uint8_t> make_modify(uint64_t seq, uint64_t order_id,
                                         uint32_t new_qty, uint16_t symbol_id) {
    WireHeader hdr{};
    hdr.magic = kMagic; hdr.version = 1; hdr.msg_type = 3; hdr.seq_num = seq;
    WireModifyOrder body{};
    body.order_id = order_id; body.new_qty = new_qty; body.symbol_id = symbol_id;
    hdr.payload_len = sizeof(body);

    std::vector<uint8_t> buf(sizeof(hdr) + sizeof(body));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), &body, sizeof(body));
    return buf;
}

static std::vector<uint8_t> make_heartbeat(uint64_t seq) {
    WireHeader hdr{};
    hdr.magic = kMagic; hdr.version = 1; hdr.msg_type = 99; hdr.seq_num = seq;
    hdr.payload_len = 0;
    std::vector<uint8_t> buf(sizeof(hdr));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    return buf;
}

// ── Decode correctness ────────────────────────────────────────────────────────

static void test_decode_new_order() {
    printf("MarketDataIngestion: decodes NewOrder correctly:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);

    auto wire = make_new_order(1, /*order_id=*/42, /*price_fp=*/to_price(150.25),
                                /*qty=*/300, /*symbol=*/7, /*side=*/0 /*buy*/,
                                /*order_type=*/0 /*limit*/);
    int n = mdi.ingest(wire);
    CHECK(n == 1, "ingest() reports one message decoded");

    MarketDataMsg msg{};
    CHECK(q.try_pop(msg), "decoded message is pushed to the outbound queue");
    CHECK(msg.msg_type == MarketDataMsg::Type::NewOrder, "msg_type is NewOrder");
    CHECK(msg.order_id == 42, "order_id round-trips exactly");
    CHECK(msg.price == to_price(150.25), "price round-trips at the same fixed-point scale (both * 10^6)");
    CHECK(msg.qty == 300, "qty round-trips exactly");
    CHECK(msg.symbol == 7, "symbol round-trips exactly");
    CHECK(msg.side == Side::Buy, "side=0 decodes to Buy");
    CHECK(msg.order_type == OrderType::Limit, "order_type=0 decodes to Limit");
    CHECK(msg.seq == 1, "seq is carried from the wire header");
    CHECK(mdi.messages_ingested() == 1, "messages_ingested() counts this message");
}

static void test_decode_sell_side() {
    printf("MarketDataIngestion: side=1 decodes to Sell:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);
    auto wire = make_new_order(1, 1, to_price(1.0), 1, 0, /*side=*/1, 0);
    (void)mdi.ingest(wire);
    MarketDataMsg msg{};
    (void)q.try_pop(msg);
    CHECK(msg.side == Side::Sell, "side=1 decodes to Sell");
}

static void test_decode_cancel() {
    printf("MarketDataIngestion: decodes CancelOrder correctly:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);
    auto wire = make_cancel(1, /*order_id=*/99, /*symbol=*/3);
    CHECK(mdi.ingest(wire) == 1, "ingest() reports one message decoded");
    MarketDataMsg msg{};
    (void)q.try_pop(msg);
    CHECK(msg.msg_type == MarketDataMsg::Type::CancelOrder, "msg_type is CancelOrder");
    CHECK(msg.order_id == 99, "order_id round-trips");
    CHECK(msg.symbol == 3, "symbol round-trips");
}

static void test_decode_modify() {
    printf("MarketDataIngestion: decodes ModifyOrder correctly:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);
    auto wire = make_modify(1, /*order_id=*/55, /*new_qty=*/777, /*symbol=*/2);
    CHECK(mdi.ingest(wire) == 1, "ingest() reports one message decoded");
    MarketDataMsg msg{};
    (void)q.try_pop(msg);
    CHECK(msg.msg_type == MarketDataMsg::Type::ModifyOrder, "msg_type is ModifyOrder");
    CHECK(msg.order_id == 55, "order_id round-trips");
    CHECK(msg.qty == 777, "new qty round-trips");
}

static void test_decode_heartbeat() {
    printf("MarketDataIngestion: decodes Heartbeat with zero-length payload:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);
    auto wire = make_heartbeat(5);
    CHECK(mdi.ingest(wire) == 1, "ingest() reports one message decoded");
    MarketDataMsg msg{};
    (void)q.try_pop(msg);
    CHECK(msg.msg_type == MarketDataMsg::Type::Heartbeat, "msg_type is Heartbeat");
}

// ── Malformed / adversarial input ─────────────────────────────────────────────

static void test_rejects_bad_magic() {
    printf("MarketDataIngestion: rejects a bad magic number:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);
    auto wire = make_new_order(1, 1, to_price(1.0), 1, 0, 0, 0);
    wire[0] ^= 0xFF; // corrupt the magic's first byte
    CHECK(mdi.ingest(wire) == 0, "ingest() decodes nothing when magic is wrong");
    CHECK(mdi.parse_errors() == 1, "parse_errors() counts the bad-magic rejection");
}

static void test_rejects_truncated_header() {
    printf("MarketDataIngestion: rejects a buffer shorter than the header:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);
    std::vector<uint8_t> too_short(sizeof(WireHeader) - 1, 0);
    CHECK(mdi.ingest(too_short) == 0, "ingest() decodes nothing when shorter than a header");
    CHECK(mdi.parse_errors() == 1, "parse_errors() counts it");
}

static void test_rejects_truncated_payload() {
    printf("MarketDataIngestion: rejects a payload shorter than declared:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);
    auto wire = make_new_order(1, 1, to_price(1.0), 1, 0, 0, 0);
    wire.resize(wire.size() - 4); wire.shrink_to_fit(); // force real reallocation, no leftover capacity
    CHECK(mdi.ingest(wire) == 0, "ingest() decodes nothing when the payload is truncated");
    CHECK(mdi.parse_errors() == 1, "parse_errors() counts it");
}

static void test_rejects_unknown_msg_type() {
    printf("MarketDataIngestion: rejects an unrecognised message type:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);
    WireHeader hdr{};
    hdr.magic = kMagic; hdr.version = 1; hdr.msg_type = 0xBEEF; hdr.seq_num = 1;
    hdr.payload_len = 0;
    std::vector<uint8_t> buf(sizeof(hdr));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    CHECK(mdi.ingest(buf) == 0, "ingest() decodes nothing for an unknown msg_type");
    CHECK(mdi.parse_errors() == 1, "parse_errors() counts it");
}

// ── Sequence gap detection ────────────────────────────────────────────────────

static void test_gap_detection_fires_callback() {
    printf("MarketDataIngestion: sequence gap fires the gap callback exactly once:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);

    int gap_calls = 0;
    uint64_t last_expected = 0, last_got = 0;
    mdi.on_gap([&](uint64_t expected, uint64_t got) {
        ++gap_calls; last_expected = expected; last_got = got;
    });

    // First message establishes the baseline — no gap check on message 1
    // (expected_seq_ starts at 0, and the implementation treats 0 as
    // "no baseline yet," not as "expected seq 0").
    (void)mdi.ingest(make_new_order(1, 1, to_price(1.0), 1, 0, 0, 0));
    CHECK(gap_calls == 0, "no gap fires on the very first message");

    (void)mdi.ingest(make_new_order(2, 2, to_price(1.0), 1, 0, 0, 0));
    CHECK(gap_calls == 0, "no gap fires on a genuinely sequential message");

    // Jump from seq 2 to seq 10 — a real gap.
    (void)mdi.ingest(make_new_order(10, 3, to_price(1.0), 1, 0, 0, 0));
    CHECK(gap_calls == 1, "gap callback fires exactly once for the jump");
    CHECK(last_expected == 3, "gap callback reports the sequence that was actually expected");
    CHECK(last_got == 10, "gap callback reports the sequence that actually arrived");
    CHECK(mdi.gaps_detected() == 1, "gaps_detected() counter matches");
}

static void test_reset_sequence_clears_gap_baseline() {
    printf("MarketDataIngestion: reset_sequence() clears the gap-detection baseline:\n");
    MarketDataIngestion::OutboundQueue q;
    MarketDataIngestion mdi(q);
    int gap_calls = 0;
    mdi.on_gap([&](uint64_t, uint64_t) { ++gap_calls; });

    (void)mdi.ingest(make_new_order(1, 1, to_price(1.0), 1, 0, 0, 0));
    mdi.reset_sequence();
    // After reset, a big jump should NOT fire a gap — same "no baseline
    // yet" rule as the very first message ever.
    (void)mdi.ingest(make_new_order(500, 2, to_price(1.0), 1, 0, 0, 0));
    CHECK(gap_calls == 0, "no gap fires immediately after reset_sequence()");
}

int main() {
    printf("=== MarketDataIngestion Tests ===\n\n");
    test_decode_new_order();
    test_decode_sell_side();
    test_decode_cancel();
    test_decode_modify();
    test_decode_heartbeat();
    test_rejects_bad_magic();
    test_rejects_truncated_header();
    test_rejects_truncated_payload();
    test_rejects_unknown_msg_type();
    test_gap_detection_fires_callback();
    test_reset_sequence_clears_gap_baseline();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
