#include "core/matching_engine.hpp"
#include <pthread.h>
#include <sched.h>
#include <cstring>

namespace engine {

MatchingEngine::MatchingEngine() {
    // unique_ptr array is value-initialized to nullptr by default.
}

MatchingEngine::~MatchingEngine() {
    stop();
}

bool MatchingEngine::register_symbol(SymbolId id) {
    if (id >= kMaxSymbols) return false;
    if (books_[id]) return false;  // already registered

    books_[id] = std::make_unique<OrderBook>(
        id,
        OrderBook::MatchCallback(this, [](void* ctx, const ExecutionReport& rpt) {
            static_cast<MatchingEngine*>(ctx)->on_execution(rpt);
        })
    );
    return true;
}

void MatchingEngine::start(int cpu_id) {
    // release: ensures the engine_thread_ sees all state written before start().
    // The run_loop reads running_ with relaxed, relying on the SPSC acquire for
    // payload synchronisation — seq_cst is unnecessary overhead here.
    running_.store(true, std::memory_order_release);
    engine_thread_ = std::thread([this] { run_loop(); });

    pin_cpu_id_ = cpu_id;
    if (cpu_id >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        const int rc = pthread_setaffinity_np(engine_thread_.native_handle(), sizeof(cpuset), &cpuset);
        // pthread_* functions return the error code directly (not -1/errno) —
        // the previous version of this function discarded this value
        // entirely, so a pin request that silently failed (e.g. EINVAL for
        // a cpu_id that doesn't exist on this machine) looked identical to
        // one that succeeded to every caller and every benchmark. Nothing
        // downstream should trust "the thread is pinned" without checking
        // is_pinned() after this call.
        pinned_    = (rc == 0);
        pin_errno_ = rc;
    } else {
        pinned_    = false;
        pin_errno_ = 0;
    }

    // SCHED_FIFO is gated on pinned_ — whether pinning actually succeeded
    // — not merely on whether cpu_id >= 0 was requested. An earlier
    // version of this function gated on the request instead of the
    // outcome, which left exactly the gap this whole check exists to
    // close: request cpu_id=1 on a machine that only has core 0 (this
    // project's own sandbox), and pinning fails while SCHED_FIFO still
    // got applied — confirmed directly, bench_replay.cpp printed
    // "pinned=no realtime=yes" on this sandbox. SCHED_FIFO is a
    // real-time policy: once running, a thread keeps the CPU until it
    // blocks or a higher-priority real-time thread preempts it — it is
    // NOT time-sliced against other threads at the same priority the way
    // the default scheduler is. Verified directly: two busy-poll
    // SCHED_FIFO threads at the same priority on one CPU produced a
    // ~8,800:1 split of scheduled iterations in 300ms (53,084,146 vs
    // 6,032) — not a deadlock, but severe enough that a thread with
    // SCHED_FIFO priority but no dedicated core can starve whatever else
    // is sharing that core, exactly the failure mode a failed pin
    // request should never be allowed to reach. A caller whose pin
    // request didn't actually succeed does not have a dedicated core,
    // regardless of what it originally asked for — applying SCHED_FIFO
    // anyway in that situation is how you get the failure mode above,
    // not how you avoid it. See docs/design.md §5 and PROFILING.md §4
    // for the full writeup.
    if (pinned_) {
        sched_param sp{ .sched_priority = 50 };
        const int rc2 = pthread_setschedparam(engine_thread_.native_handle(), SCHED_FIFO, &sp);
        sched_fifo_active_ = (rc2 == 0);
        sched_errno_        = rc2;
    } else {
        sched_fifo_active_  = false;
        sched_errno_        = 0;
    }
}

void MatchingEngine::stop() {
    // release: the engine thread's relaxed load will eventually see this.
    // We then join(), which provides a full happens-before fence anyway.
    running_.store(false, std::memory_order_release);
    if (engine_thread_.joinable())
        engine_thread_.join();
}

void MatchingEngine::run_loop() noexcept {
    MarketDataMsg msg;

    while (running_.load(std::memory_order_relaxed)) {
        // Busy-poll: no condition variable, no mutex.
        // The SPSC queue's acquire-load is the only synchronization.
        if (inbound_.try_pop(msg)) [[likely]] {
            process_message(msg);
            stat_messages_.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Yield occasionally to avoid monopolizing the CPU when idle.
            // In a production system you'd typically stay on a dedicated
            // isolated core and never yield.
            __builtin_ia32_pause();  // PAUSE hint: reduces power, tells CPU we're spinning
        }
    }

    // Drain remaining messages on shutdown.
    while (inbound_.try_pop(msg))
        process_message(msg);
}

void MatchingEngine::process_message(const MarketDataMsg& msg) noexcept {
    if (msg.symbol >= kMaxSymbols) return;
    OrderBook* book = books_[msg.symbol].get();
    if (!book) return;

    switch (msg.msg_type) {
        case MarketDataMsg::Type::NewOrder: {
            Order o{};
            o.id            = msg.order_id ? msg.order_id : next_order_id_++;
            o.price         = msg.price;
            o.qty           = msg.qty;
            o.qty_remaining = msg.qty;
            o.symbol        = msg.symbol;
            o.side          = msg.side;
            o.type          = msg.order_type;
            o.status        = OrderStatus::New;
            book->add_order(o);
            break;
        }
        case MarketDataMsg::Type::CancelOrder:
            book->cancel_order(msg.order_id);
            break;

        case MarketDataMsg::Type::ModifyOrder:
            book->modify_order(msg.order_id, msg.qty);
            break;

        case MarketDataMsg::Type::Heartbeat:
            // No-op: used to verify queue liveness.
            break;
    }
}

void MatchingEngine::on_execution(const ExecutionReport& rpt) noexcept {
    stat_matches_.fetch_add(1, std::memory_order_relaxed);
    // Best-effort enqueue: if the outbound queue is full, drop the report
    // and let the execution layer detect missing fills via sequence gaps.
    static_cast<void>(outbound_.try_push(rpt));
}

} // namespace engine
