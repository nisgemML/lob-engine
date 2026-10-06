#pragma once

#include "core/types.hpp"
#include "util/function_ref.hpp"
#include "util/allocator.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace engine {

struct PriceLevel {
    Price    price;
    Qty      total_qty;
    uint32_t order_count;
    uint32_t head_idx;
};

static constexpr uint32_t NULL_IDX = UINT32_MAX;

class OrderBook {
public:
    static constexpr std::size_t kMaxLevels = 4096;
    static constexpr std::size_t kMaxOrders = 65536;

    // Non-owning: the callable must outlive the book. See util/function_ref.hpp.
    using MatchCallback = util::function_ref<void(const ExecutionReport&)>;

    explicit OrderBook(SymbolId symbol, MatchCallback on_match);

    bool add_order(const Order& order) noexcept;
    bool cancel_order(OrderId order_id) noexcept;
    bool modify_order(OrderId order_id, Qty new_qty) noexcept;

    [[nodiscard]] BestQuote best_quote() const noexcept;
    [[nodiscard]] std::size_t bid_depth(std::span<PriceLevel> out) const noexcept;
    [[nodiscard]] std::size_t ask_depth(std::span<PriceLevel> out) const noexcept;

    [[nodiscard]] SymbolId  symbol()          const noexcept { return symbol_; }
    [[nodiscard]] uint64_t  total_orders()    const noexcept { return stat_orders_; }
    [[nodiscard]] uint64_t  total_matches()   const noexcept { return stat_matches_; }
    [[nodiscard]] uint64_t  total_cancelled() const noexcept { return stat_cancelled_; }
    [[nodiscard]] uint64_t  total_self_trade_prevented() const noexcept { return stat_self_trade_prevented_; }

    // Diagnostic (O(table size), not for the hot path): length of the longest
    // run of occupied slots in the order-id index. Linear-probing cost is
    // bounded by it, so tests assert on it directly instead of on timing.
    [[nodiscard]] uint32_t index_longest_cluster() const noexcept { return index_.longest_cluster(); }

    // ── Internal types exposed for the .cpp implementation ──────────────────

    struct Side {
        // Struct-of-arrays — prices[] is the hot field iterated during matching.
        std::array<Price,     kMaxLevels> prices;
        std::array<Qty,       kMaxLevels> qtys;
        std::array<uint32_t,  kMaxLevels> order_counts;
        std::array<uint32_t,  kMaxLevels> head_idxs;
        // tail_idxs: index of the last order slot at each level.
        // Maintained alongside head_idxs so append is O(1) instead of O(n).
        // Invariant: if head_idxs[i] == NULL_IDX then tail_idxs[i] == NULL_IDX.
        std::array<uint32_t,  kMaxLevels> tail_idxs;
        uint32_t count = 0;

        [[nodiscard]] int32_t find_level(Price p, bool descending) const noexcept;
        int32_t insert_level(Price p, bool descending) noexcept;
        void remove_level(uint32_t idx) noexcept;
    };

    struct SlotPool {
        // Parallel arrays for each order slot. No cache-line padding here
        // (contrast with mpmc_queue.hpp's alignas(64) counters and
        // multi_symbol_engine.hpp's alignas(64) shard fields): those pad
        // against false sharing between *threads*. OrderBook itself is
        // single-threaded per instance by design — one thread owns one
        // book — so no other thread is ever touching an adjacent slot in
        // these arrays concurrently, and padding would only cost L1
        // density for no correctness or latency benefit. If that
        // single-writer invariant ever changes (e.g. a lock-free
        // multi-producer book), this comment is the place to revisit it.
        std::array<OrderId,  kMaxOrders> ids;
        std::array<Qty,      kMaxOrders> qtys;
        std::array<uint32_t, kMaxOrders> nexts;       // live doubly-linked list: next
        std::array<uint32_t, kMaxOrders> prevs;       // live doubly-linked list: prev
        std::array<uint32_t, kMaxOrders> free_nexts;  // free-list chain (separate from nexts)
        // Store price (not level index) — stable across remove_level() shifts.
        std::array<Price,    kMaxOrders> prices;
        // Self-trade-prevention key, mirrors Order::account_id. Only
        // meaningful for an allocated slot; read during try_match().
        std::array<uint32_t, kMaxOrders> accounts;

        uint32_t free_head = 0;
        uint32_t used      = 0;

        void     init() noexcept;
        [[nodiscard]] uint32_t alloc() noexcept;
        void     free_slot(uint32_t idx) noexcept;
    };

    struct OrderIndex {
        static constexpr uint32_t kTableSize = kMaxOrders * 2;   // load factor <= 0.5
        static constexpr uint32_t kBits      = 17;
        static constexpr uint32_t kMask      = kTableSize - 1;
        static_assert(kTableSize == (1u << kBits), "Fibonacci hash keeps kBits top bits; table must be 2^kBits");
        static constexpr uint32_t kEmpty     = UINT32_MAX;

        struct Entry {
            OrderId  key   = 0;
            uint32_t slot  = kEmpty;
            uint8_t  side  = 0xFF;
        };

        std::array<Entry, kTableSize> table;

        void init() noexcept;
        bool insert(OrderId id, uint32_t slot, uint8_t side) noexcept;
        bool lookup(OrderId id, uint32_t& slot_out, uint8_t& side_out) const noexcept;
        bool remove(OrderId id) noexcept;
        [[nodiscard]] uint32_t longest_cluster() const noexcept;   // diagnostic

    private:
        [[nodiscard]] static uint32_t home(OrderId id) noexcept;
        [[nodiscard]] uint32_t probe(OrderId id) const noexcept;
    };

private:
    bool try_match(Order& incoming) noexcept;   // returns true if it stopped on an STP wall
    [[nodiscard]] Qty available_liquidity(const Order& incoming) const noexcept;
    void remove_order_from_level(uint8_t side_idx, uint32_t level_idx,
                                 uint32_t slot) noexcept;

    MatchCallback match_cb_;
    SymbolId      symbol_;

    Side      bids_;
    Side      asks_;
    SlotPool  slots_;
    OrderIndex index_;

    uint64_t stat_orders_    = 0;
    uint64_t stat_matches_   = 0;
    uint64_t stat_cancelled_ = 0;
    uint64_t stat_rejected_  = 0;
    uint64_t stat_self_trade_prevented_ = 0;
};


#if defined(__AVX2__)
// AVX2 vectorised price level search — implementation in order_book.cpp.
// 4x int64_t per cycle (VPCMPEQQ ymm, ymm, ymm). Scalar fallback for tail.
[[nodiscard]] int32_t find_level_avx2(
    const Price* __restrict__ prices,
    uint32_t count,
    Price target) noexcept;
#endif // __AVX2__

} // namespace engine
