#pragma once

// OrderFlowReplay — reads a binary order-event trace and replays it
// through the matching engine at controlled pace or as fast as possible.
//
// Trace format (simple, not PCAP):
//   File header: magic(4) version(2) symbol_count(2) event_count(8)
//   Per event:   timestamp_ns(8) MarketDataMsg(variable via wire format)
//
// Use cases:
//   1. Deterministic latency regression testing — replay the same trace
//      before and after a change; compare latency distributions.
//   2. Worst-case scenario replay — recorded from a real market event
//      (e.g. flash crash) to stress-test the book under real order flow.
//   3. Throughput profiling — replay at maximum speed to find bottlenecks.

#include "core/types.hpp"
#include "core/matching_engine.hpp"
#include "util/histogram.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <functional>
#include <time.h>

namespace engine {

// ── Trace file structures ──────────────────────────────────────────────────

// TraceHeader and TraceEvent use explicit fixed-width fields rather than
// embedding structs directly, so the on-disk format is ABI-stable across
// compilers and platforms (no hidden padding from struct alignment).
#pragma pack(push, 1)
struct TraceHeader {
    uint32_t magic;          // 0x54524143 "TRAC"
    uint16_t version;        // 1
    uint16_t symbol_count;
    uint64_t event_count;
    uint64_t start_ts_ns;
    uint8_t  _pad[8];
};

// Serialised event: all fields explicit, no embedded structs.
struct TraceEvent {
    uint64_t timestamp_ns;
    uint64_t seq;
    uint64_t order_id;
    int64_t  price;
    uint32_t qty;
    uint16_t symbol;
    uint8_t  side;          // 0=buy 1=sell
    uint8_t  order_type;    // 0=limit 1=market 2=ioc 3=fok
    uint8_t  msg_type;      // 0=new 1=cancel 2=modify 3=heartbeat
    uint8_t  _pad[7];
};
static_assert(sizeof(TraceEvent) == 48, "TraceEvent must be 48 bytes");
#pragma pack(pop)

// Convert between TraceEvent (stable on-disk) and MarketDataMsg (in-memory).
inline MarketDataMsg trace_event_to_msg(const TraceEvent& e) noexcept {
    MarketDataMsg m{};
    m.seq        = e.seq;
    m.order_id   = e.order_id;
    m.price      = static_cast<Price>(e.price);
    m.qty        = e.qty;
    m.symbol     = e.symbol;
    m.side       = (e.side == 0) ? Side::Buy : Side::Sell;
    m.order_type = static_cast<OrderType>(e.order_type);
    m.msg_type   = static_cast<MarketDataMsg::Type>(e.msg_type);
    return m;
}

inline TraceEvent msg_to_trace_event(uint64_t ts_ns, const MarketDataMsg& m) noexcept {
    TraceEvent e{};
    e.timestamp_ns = ts_ns;
    e.seq          = m.seq;
    e.order_id     = m.order_id;
    e.price        = static_cast<int64_t>(m.price);
    e.qty          = m.qty;
    e.symbol       = m.symbol;
    e.side         = (m.side == Side::Buy) ? 0 : 1;
    e.order_type   = static_cast<uint8_t>(m.order_type);
    e.msg_type     = static_cast<uint8_t>(m.msg_type);
    return e;
}

static constexpr uint32_t kTraceMagic = 0x54524143;

// ── Trace writer ────────────────────────────────────────────────────────────

class TraceWriter {
public:
    explicit TraceWriter(const char* path) {
        f_ = std::fopen(path, "wb");
        if (!f_) return;

        TraceHeader hdr{};
        hdr.magic   = kTraceMagic;
        hdr.version = 1;
        std::fwrite(&hdr, sizeof(hdr), 1, f_);
    }

    ~TraceWriter() {
        if (f_) {
            // Patch event count into header.
            std::fseek(f_, offsetof(TraceHeader, event_count), SEEK_SET);
            std::fwrite(&event_count_, sizeof(event_count_), 1, f_);
            std::fclose(f_);
        }
    }

    bool write(uint64_t ts_ns, const MarketDataMsg& msg) {
        if (!f_) return false;
        TraceEvent ev = msg_to_trace_event(ts_ns, msg);
        std::fwrite(&ev, sizeof(ev), 1, f_);
        ++event_count_;
        return true;
    }

    [[nodiscard]] uint64_t events_written() const noexcept { return event_count_; }

private:
    FILE*    f_           = nullptr;
    uint64_t event_count_ = 0;
};

// ── Replay result ───────────────────────────────────────────────────────────

struct ReplayResult {
    // Default member initializers are load-bearing here, not decorative:
    // `ReplayResult result;` at both call sites in bench_replay.cpp is a
    // plain default-construction, and for an aggregate with no
    // initializers, that leaves these three counters as indeterminate
    // stack garbage until replay()'s `++result.events_replayed` etc. add
    // 1 to whatever garbage was already there — never actually starting
    // from zero. Confirmed directly: a run printed "Events replayed:
    // 32400697324457" against a 500,000-event trace, which is only
    // possible if the counter started somewhere other than zero.
    uint64_t events_replayed  = 0;
    uint64_t events_skipped   = 0;    // e.g. unknown symbol
    uint64_t matches_generated = 0;
    LatencyHistogram latency;   // submit-to-first-report latency per event

    void print() const noexcept {
        printf("Replay complete:\n");
        printf("  Events replayed  : %lu\n", events_replayed);
        printf("  Events skipped   : %lu\n", events_skipped);
        printf("  Matches          : %lu\n", matches_generated);
        auto snap = latency.take_snapshot();
        snap.print("Submit latency");
        snap.print_histogram();
    }
};

// ── Replay engine ────────────────────────────────────────────────────────────

class OrderFlowReplay {
public:
    enum class Mode {
        MaxSpeed,       // replay as fast as possible (throughput test)
        WallClock,      // replay at original timestamps (latency test)
        Throttled,      // replay at a fixed rate (msgs/sec)
    };

    struct Config {
        Mode     mode          = Mode::MaxSpeed;
        uint64_t throttle_mps = 1'000'000;   // msgs/sec for Throttled mode
        bool     verbose       = false;
        uint32_t warmup_events = 10'000;     // events to skip before measuring
    };

    explicit OrderFlowReplay(MatchingEngine& engine, Config cfg)
        : engine_(engine), cfg_(cfg) {}

    // Replay a trace file. Fills result. Returns false on open/parse failure.
    bool replay(const char* trace_path, ReplayResult& result) noexcept {
        FILE* f = std::fopen(trace_path, "rb");
        if (!f) return false;

        TraceHeader hdr{};
        if (std::fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != kTraceMagic) {
            std::fclose(f);
            return false;
        }

        const uint64_t n = hdr.event_count;
        uint64_t first_ts = 0;
        uint64_t wall_start = now_ns();

        for (uint64_t i = 0; i < n; ++i) {
            TraceEvent ev{};
            if (std::fread(&ev, sizeof(ev), 1, f) != 1) break;

            if (i == 0) first_ts = ev.timestamp_ns;

            MarketDataMsg msg = trace_event_to_msg(ev);

            // Timing control.
            if (cfg_.mode == Mode::WallClock && i > 0) {
                const uint64_t target_elapsed = ev.timestamp_ns - first_ts;
                const uint64_t actual_elapsed = now_ns() - wall_start;
                if (target_elapsed > actual_elapsed) {
                    struct timespec ts{
                        .tv_sec  = 0,
                        .tv_nsec = static_cast<long>(target_elapsed - actual_elapsed)
                    };
                    nanosleep(&ts, nullptr);
                }
            } else if (cfg_.mode == Mode::Throttled) {
                const uint64_t min_gap_ns = 1'000'000'000ULL / cfg_.throttle_mps;
                const uint64_t expected   = wall_start + i * min_gap_ns;
                const uint64_t now        = now_ns();
                if (expected > now) {
                    struct timespec ts{ .tv_sec = 0, .tv_nsec = static_cast<long>(expected - now) };
                    nanosleep(&ts, nullptr);
                }
            }

            // Submit.
            const uint64_t t0 = now_ns();
            bool submitted = false;
            for (int spin = 0; spin < 1000; ++spin) {
                if (engine_.submit(msg)) { submitted = true; break; }
                __builtin_ia32_pause();
            }

            if (!submitted) { ++result.events_skipped; continue; }

            // Poll for first execution report (non-blocking).
            ExecutionReport rpt;
            if (i >= cfg_.warmup_events && engine_.poll_report(rpt)) {
                result.latency.record(now_ns() - t0);
                ++result.matches_generated;
            }

            ++result.events_replayed;

            if (cfg_.verbose && i % 100'000 == 0)
                printf("  [replay] %lu / %lu events\n", i, n);
        }

        // Drain remaining reports.
        ExecutionReport rpt;
        while (engine_.poll_report(rpt)) ++result.matches_generated;

        std::fclose(f);
        return true;
    }

private:
    static uint64_t now_ns() noexcept {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
    }

    MatchingEngine& engine_;
    Config          cfg_;
};

} // namespace engine
