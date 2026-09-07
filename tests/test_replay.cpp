// tests/test_replay.cpp — Tests for TraceWriter/OrderFlowReplay
// (include/core/replay.hpp), previously entirely untested.
//
// The concrete bug this file's first test guards against: ReplayResult's
// three uint64_t counters (events_replayed, events_skipped,
// matches_generated) had no default member initializers, and every call
// site declares `ReplayResult result;` — a plain default-construction
// that, for an aggregate with no initializers, leaves those counters as
// indeterminate stack garbage until replay()'s `++result.events_replayed`
// adds 1 to whatever garbage was already there. Confirmed directly: a
// real run printed "Events replayed: 32400697324457" against a
// 500,000-event trace. Fixed with `= 0` default member initializers;
// this test asserts a fresh, unused ReplayResult reads back as exactly
// zero on every counter, and that replayed + skipped equals the trace's
// true event count after a real run.

#include "core/replay.hpp"
#include "core/matching_engine.hpp"
#include <cstdio>
#include <cstdlib>

using namespace engine;

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); ++failed; } \
        else { ++passed; } \
    } while (0)

static void test_fresh_replay_result_is_zeroed() {
    printf("ReplayResult: a freshly-constructed instance reads back as zero:\n");
    // This test reliably PASSES with the fix in place (default member
    // initializers give a hard guarantee — the standard, not luck, says
    // these read as 0). It is honest to note what it does NOT reliably
    // do: catch a regression back to uninitialized members. An attempt to
    // force that deterministically (deliberately dirtying a stack region
    // before this declaration) did not reproduce the bug in this binary —
    // reading an uninitialized value is undefined behavior, and whether a
    // given stack slot happens to hold nonzero garbage depends on prior
    // stack usage in ways the optimizer doesn't guarantee this test can
    // control. The authoritative reproduction of the actual bug is a real
    // run of bench_replay.cpp (a much deeper, more realistic call stack)
    // printing "Events replayed: 32400697324457" against a 500,000-event
    // trace — recorded here, not re-derived by this test on demand.
    ReplayResult r;
    CHECK(r.events_replayed == 0, "events_replayed starts at 0, not garbage");
    CHECK(r.events_skipped == 0, "events_skipped starts at 0, not garbage");
    CHECK(r.matches_generated == 0, "matches_generated starts at 0, not garbage");
    CHECK(r.latency.count() == 0, "latency histogram starts empty");
}

static void test_trace_write_and_replay_round_trip() {
    printf("TraceWriter -> OrderFlowReplay: event counts are exact and bounded:\n");
    const char* path = "/tmp/test_replay_trace.bin";
    constexpr uint64_t kEvents = 20'000;

    {
        TraceWriter writer(path);
        for (uint64_t i = 0; i < kEvents; ++i) {
            MarketDataMsg msg{};
            msg.msg_type   = MarketDataMsg::Type::NewOrder;
            msg.order_id   = i + 1;
            msg.symbol     = 0;
            msg.side       = (i % 2) ? Side::Buy : Side::Sell;
            msg.price      = to_price(100.0);
            msg.qty        = 10;
            msg.order_type = OrderType::Limit;
            writer.write(i * 1000, msg);
        }
        CHECK(writer.events_written() == kEvents, "TraceWriter reports the exact count written");
    }

    MatchingEngine engine;
    engine.register_symbol(0);
    engine.start();

    OrderFlowReplay::Config cfg;
    cfg.mode = OrderFlowReplay::Mode::MaxSpeed;
    OrderFlowReplay replay(engine, cfg);
    ReplayResult result;
    bool ok = replay.replay(path, result);
    engine.stop();

    CHECK(ok, "replay() succeeds on a well-formed trace");
    // This is the exact regression check: before the fix, this sum could
    // be anything (garbage + kEvents), never reliably equal to kEvents.
    CHECK(result.events_replayed + result.events_skipped == kEvents,
          "replayed + skipped == exactly the number of events written — "
          "would only hold by coincidence if either counter started "
          "anywhere other than zero");
    CHECK(result.events_replayed <= kEvents, "replayed count is never more than the trace holds");
}

static void test_replay_rejects_bad_magic() {
    printf("OrderFlowReplay: a file with a wrong/missing magic is rejected, not crashed on:\n");
    const char* path = "/tmp/test_replay_badmagic.bin";
    FILE* f = fopen(path, "wb");
    CHECK(f != nullptr, "throwaway file created");
    if (f) {
        uint32_t garbage = 0xDEADBEEF;
        fwrite(&garbage, sizeof(garbage), 1, f);
        fclose(f);
    }

    MatchingEngine engine;
    engine.register_symbol(0);
    engine.start();
    OrderFlowReplay replay(engine, {});
    ReplayResult result;
    bool ok = replay.replay(path, result);
    engine.stop();

    CHECK(!ok, "replay() returns false for a file with a bad/missing magic, not garbage results");
}

int main() {
    printf("=== OrderFlowReplay / TraceWriter Tests ===\n\n");
    test_fresh_replay_result_is_zeroed();
    test_trace_write_and_replay_round_trip();
    test_replay_rejects_bad_magic();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
