#pragma once

// MatchingEngine — orchestrates order books and routes execution reports.
//
// Threading model:
//   The matching engine runs on a single dedicated CPU core, pinned via
//   pthread_setaffinity_np.  All order processing is single-threaded.
//   Orders arrive via SPSC queues from the market data ingestion thread(s)
//   and execution reports leave via SPSC queues to the execution layer.
//
//   This is a deliberate design choice: the cost of a mutex acquisition
//   (CAS + memory barrier + potential OS reschedule) on the hot path dwarfs
//   the benefit of parallel matching.  LOB matching is inherently sequential
//   per symbol anyway; parallelism at the engine level adds synchronization
//   cost without proportional throughput gain.

#include "core/types.hpp"
#include "core/spsc_queue.hpp"
#include "core/order_book.hpp"
#include <array>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <memory>

namespace engine {

class MatchingEngine {
public:
    static constexpr std::size_t kQueueDepth  = 1 << 16;  // 65536 slots
    static constexpr std::size_t kMaxSymbols  = 256;
    static constexpr int         kSpinUsec    = 1;

    using InboundQueue  = SPSCQueue<MarketDataMsg, kQueueDepth>;
    using OutboundQueue = SPSCQueue<ExecutionReport, kQueueDepth>;

    MatchingEngine();
    ~MatchingEngine();

    // Not copyable or movable — owns threads and shared queues.
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // Register a symbol before starting.
    bool register_symbol(SymbolId id);

    // Start/stop the engine thread.
    //
    // cpu_id selects which core the matching thread is pinned to via
    // pthread_setaffinity_np (default 1, preserving this class's original
    // hard-coded behavior for every existing caller). Pass -1 to request
    // no pinning at all. Both the pin and the SCHED_FIFO elevation record
    // whether they actually took effect — see is_pinned()/is_realtime()
    // below — instead of assuming success.
    //
    // SCHED_FIFO is gated on whether the pin actually succeeded (is_pinned()
    // after the attempt), not merely on whether cpu_id >= 0 was requested.
    // An earlier version of this function gated on the request instead of
    // the outcome: on a machine with fewer cores than a hard-coded cpu_id
    // assumes, pinning fails with EINVAL but SCHED_FIFO still got applied
    // — confirmed directly, a benchmark run on a single-core sandbox
    // printed "pinned=no realtime=yes". SCHED_FIFO is a real-time policy —
    // a thread keeps the CPU until it blocks or a higher-priority
    // real-time thread preempts it, unlike the default scheduler's
    // time-slicing. Two busy-poll SCHED_FIFO threads at the same priority
    // sharing one CPU (measured directly, not assumed) split scheduled
    // iterations roughly 8,800:1, not evenly — severe, though not a
    // deadlock. A thread with SCHED_FIFO priority but no dedicated core
    // is exactly this failure mode waiting to happen, regardless of
    // whether the caller explicitly passed cpu_id=-1 or asked for a core
    // that turned out not to exist — a failed pin request means there is
    // no dedicated core either way, and applying SCHED_FIFO anyway is how
    // you get the failure mode above, not how you avoid it. See
    // docs/design.md §5 and PROFILING.md §4.
    void start(int cpu_id = 1);
    void stop();

    // Enqueue an inbound message (called from producer thread).
    [[nodiscard]] bool submit(const MarketDataMsg& msg) noexcept {
        return inbound_.try_push(msg);
    }

    // Drain one execution report (called from consumer thread).
    [[nodiscard]] bool poll_report(ExecutionReport& out) noexcept {
        return outbound_.try_pop(out);
    }

    // True only if pthread_setaffinity_np actually succeeded for the
    // cpu_id passed to start() — not merely that it was requested. A
    // latency claim made without checking this is a claim about what was
    // asked for, not what was measured under.
    [[nodiscard]] bool is_pinned() const noexcept { return pinned_; }
    [[nodiscard]] int  pin_cpu_id() const noexcept { return pin_cpu_id_; }
    [[nodiscard]] int  pin_errno() const noexcept { return pin_errno_; }

    // True only if pthread_setschedparam(SCHED_FIFO) actually succeeded —
    // typically requires CAP_SYS_NICE or root. sched_errno() carries the
    // errno (commonly EPERM) when it didn't.
    [[nodiscard]] bool is_realtime() const noexcept { return sched_fifo_active_; }
    [[nodiscard]] int  sched_errno() const noexcept { return sched_errno_; }

    // Statistics (approximate, no lock).
    [[nodiscard]] uint64_t messages_processed() const noexcept {
        return stat_messages_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t matches_generated() const noexcept {
        return stat_matches_.load(std::memory_order_relaxed);
    }

    // Read-only access to a registered symbol's book — nullptr if the
    // symbol was never registered. Intended for monitoring/introspection
    // (e.g. a recovery/restart check that the reconstructed book matches
    // what existed before a restart) — not for the hot path, and not a
    // way to mutate book state outside the engine's own single-threaded
    // matching loop.
    //
    // NOT safe to call while the engine is running, from a different
    // thread, without external synchronization — the matching thread
    // (per this class's single-threaded-matching design, see
    // docs/design.md §1) continues writing to this OrderBook
    // concurrently, with no lock protecting it. Confirmed directly under
    // ThreadSanitizer while building examples/recovery_demo.cpp: calling
    // best_quote() on the returned pointer from the main thread, based on
    // a sleep_for() "probably done by now" assumption rather than an
    // actual synchronization point, raced against the matching thread's
    // own OrderBook writes. Call stop() first — its thread::join()
    // provides the real happens-before guarantee a sleep does not — or
    // otherwise guarantee the matching thread cannot be concurrently
    // active when you read from the returned pointer.
    [[nodiscard]] const OrderBook* book_for(SymbolId id) const noexcept {
        if (id >= kMaxSymbols) return nullptr;
        return books_[id].get();
    }

private:
    void run_loop() noexcept;
    void process_message(const MarketDataMsg& msg) noexcept;
    void on_execution(const ExecutionReport& rpt) noexcept;

    InboundQueue  inbound_;
    OutboundQueue outbound_;

    std::array<std::unique_ptr<OrderBook>, kMaxSymbols> books_;

    std::thread engine_thread_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> stat_messages_{0};
    std::atomic<uint64_t> stat_matches_{0};

    bool pinned_          = false;
    int  pin_cpu_id_      = -1;
    int  pin_errno_       = 0;
    bool sched_fifo_active_ = false;
    int  sched_errno_     = 0;

    // Next synthetic order id for converted market orders.
    uint64_t next_order_id_{1};
};

} // namespace engine
