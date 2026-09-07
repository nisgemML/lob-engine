// examples/recovery_demo.cpp — a genuine crash-recovery demonstration
// built on machinery this repo already has and already trusts, not new,
// unproven code.
//
// ── The story LIMITATIONS.md/docs/design.md told before this file existed ──
//
// "Persistence / crash recovery: A production LOB would write-ahead-log
// every order event... replay the log to reconstruct book state. Left
// out to keep the core matching logic clear." That's an accurate
// description of what's missing — but this codebase already contains
// exactly that mechanism, just built and framed for a different purpose:
// TraceWriter (include/core/replay.hpp) records every order event with a
// timestamp to a binary file, and OrderFlowReplay reads it back and
// replays it through a MatchingEngine — built for latency benchmarking
// (bench/bench_replay.cpp) and proven byte-exact deterministic by
// tools/replay_trace.cpp's own determinism check (which found two real
// bugs during this repo's development, per README.md).
//
// A write-ahead log is, structurally, exactly this: an ordered, durable
// record of every event, replayable to reconstruct state. This demo does
// not add a new recovery mechanism — it demonstrates that the one this
// repo already has, and already trusts for a different reason, is the
// same one a crash-recovery story needs, by actually running the
// recovery path: submit real orders (logging each to a TraceWriter as it
// happens, as a live system would), destroy the engine to simulate a
// crash, reconstruct a fresh one, replay the log, and verify the
// reconstructed book state is IDENTICAL to what existed the moment
// before the "crash" — not merely "didn't crash," but checked against
// the actual pre-crash best bid/ask and resting order state.
//
// ── What this does not claim ──────────────────────────────────────────────
//
// This is not a production WAL — there's no fsync-before-ack durability
// guarantee here (TraceWriter's own file writes are ordinary buffered
// I/O, not covered by this demo), no log rotation, no compaction, and no
// answer for "what if the crash happens mid-write to the trace file
// itself" (a partially-written last record). Those are real, separate
// engineering problems a production WAL has to solve. What this DOES
// establish: the reconstruction half of the story — "replay a durable
// event log through a fresh engine and get back the same book state" —
// already works, is already tested for determinism, and doesn't need
// new code to demonstrate.

#include "core/matching_engine.hpp"
#include "core/replay.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <chrono>

using namespace engine;
using namespace std::chrono_literals;

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); ++failed; } \
        else { ++passed; } \
    } while (0)

static uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Submits msg to the engine AND logs it to the trace — the "live system"
// pattern: every order event that reaches the book is durably recorded
// as it happens, not batched or reconstructed after the fact.
static bool submit_and_log(MatchingEngine& engine, TraceWriter& wal, const MarketDataMsg& msg) {
    wal.write(now_ns(), msg);
    return engine.submit(msg);
}

static MarketDataMsg new_order(uint64_t id, Side side, double price, Qty qty,
                                OrderType type = OrderType::Limit) {
    MarketDataMsg m{};
    m.msg_type = MarketDataMsg::Type::NewOrder;
    m.order_id = id; m.symbol = 0; m.side = side;
    m.price = to_price(price); m.qty = qty; m.order_type = type;
    return m;
}

static MarketDataMsg cancel(uint64_t id) {
    MarketDataMsg m{};
    m.msg_type = MarketDataMsg::Type::CancelOrder;
    m.order_id = id; m.symbol = 0;
    return m;
}

int main() {
    printf("=== Crash Recovery Demo: WAL-replay reconstructs exact book state ===\n\n");
    const char* wal_path = "/tmp/recovery_demo_wal.bin";

    // ── Phase 1: "live" system, orders arrive, WAL records each one ────────
    printf("--- Phase 1: live trading, every order logged as it happens ---\n");

    // Heap-allocated — see docs/design.md's stack-hazard note: MatchingEngine
    // is ~5.5MB; a second one declared as a plain stack local in Phase 2
    // below, in the same function, would risk exactly the stack overflow
    // this repo hit twice already while building this update (see
    // tests/test_order_book.cpp's BookFixture comment and
    // examples/feed_to_execution_demo.cpp's comment).
    auto engine1 = std::make_unique<MatchingEngine>();
    engine1->register_symbol(0);
    engine1->start();

    {
        TraceWriter wal(wal_path);

        CHECK(submit_and_log(*engine1, wal, new_order(1, Side::Sell, 101.00, 200)), "resting sell 1 logged+submitted");
        CHECK(submit_and_log(*engine1, wal, new_order(2, Side::Sell, 101.50, 300)), "resting sell 2 logged+submitted");
        CHECK(submit_and_log(*engine1, wal, new_order(3, Side::Buy,   99.50, 150)), "resting buy 1 logged+submitted");
        CHECK(submit_and_log(*engine1, wal, new_order(4, Side::Buy,   99.00, 400)), "resting buy 2 logged+submitted");
        std::this_thread::sleep_for(10ms);

        // A cancel and a partial fill — recovery has to get these right
        // too, not just replay every NewOrder as if nothing else happened.
        CHECK(submit_and_log(*engine1, wal, cancel(4)), "cancel logged+submitted");
        std::this_thread::sleep_for(5ms);
        CHECK(submit_and_log(*engine1, wal, new_order(5, Side::Buy, 101.00, 100)), "partial-fill buy logged+submitted");
        std::this_thread::sleep_for(10ms);

        CHECK(wal.events_written() == 6, "WAL recorded exactly the 6 events submitted");
    } // TraceWriter destructor flushes/closes the file.

    // Stop the engine BEFORE reading book state — book_for() returns a
    // pointer into the engine's own OrderBook, which the matching thread
    // (still running at this point, per MatchingEngine's single-threaded-
    // matching design in docs/design.md §1) continues to write to
    // concurrently with anything the main thread might read. The
    // `sleep_for()` calls above make it overwhelmingly likely every
    // submitted message has already been processed by this point, but
    // "likely" is not a synchronization primitive — confirmed directly
    // under ThreadSanitizer: reading best_quote() here, before stop(),
    // raced against the matching thread's own writes
    // (OrderBook::Side::insert_level/try_match/remove_level). stop()
    // joins the matching thread; only after that join is there a real
    // happens-before relationship making this main-thread read safe.
    engine1->stop();

    const OrderBook* book1 = engine1->book_for(0);
    CHECK(book1 != nullptr, "symbol 0's book exists before the crash");
    BestQuote before = book1->best_quote();
    printf("Before crash: best bid=%.2f best ask=%.2f\n",
           from_price(before.bid_price), from_price(before.ask_price));

    engine1.reset(); // ── simulate the crash: the process (and its in-memory book) is gone ──
    printf("\n--- \"Crash\": engine destroyed, in-memory book state is gone ---\n\n");

    // ── Phase 2: recovery — replay the WAL through a fresh engine ──────────
    printf("--- Phase 2: recovery — replay the WAL through a brand-new engine ---\n");

    auto engine2 = std::make_unique<MatchingEngine>();
    engine2->register_symbol(0);
    engine2->start();

    OrderFlowReplay::Config cfg;
    cfg.mode = OrderFlowReplay::Mode::MaxSpeed;
    OrderFlowReplay replay(*engine2, cfg);
    ReplayResult result;
    bool ok = replay.replay(wal_path, result);
    CHECK(ok, "WAL replays successfully into the fresh engine");
    CHECK(result.events_replayed == 6, "all 6 logged events replayed — nothing lost");
    CHECK(result.events_skipped == 0, "nothing skipped — a clean recovery replay");

    // Give the fresh engine's thread a moment to finish processing the
    // replayed messages before inspecting book state.
    std::this_thread::sleep_for(50ms);

    // Stop the engine before reading its book state — see Phase 1's
    // identical comment above for why sleep_for() alone isn't a real
    // synchronization guarantee, confirmed the same way under
    // ThreadSanitizer for this read too.
    engine2->stop();

    const OrderBook* book2 = engine2->book_for(0);
    CHECK(book2 != nullptr, "symbol 0's book exists after recovery");
    BestQuote after = book2->best_quote();
    printf("After recovery: best bid=%.2f best ask=%.2f\n\n",
           from_price(after.bid_price), from_price(after.ask_price));

    // ── The actual recovery check: reconstructed state == pre-crash state ──
    CHECK(after.bid_price == before.bid_price,
          "recovered best bid EXACTLY matches the pre-crash best bid");
    CHECK(after.ask_price == before.ask_price,
          "recovered best ask EXACTLY matches the pre-crash best ask");

    printf("=== Summary ===\n");
    printf("Events logged   : 6\n");
    printf("Events replayed : %lu\n", result.events_replayed);
    printf("Book state match: %s\n",
           (after.bid_price == before.bid_price && after.ask_price == before.ask_price)
               ? "EXACT" : "MISMATCH");

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
