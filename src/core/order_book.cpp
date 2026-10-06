#if defined(__AVX2__)
#include <immintrin.h>
#endif
#include "core/order_book.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>

namespace engine {

// ── OrderBook::Side ───────────────────────────────────────────────────────────

int32_t OrderBook::Side::find_level(Price p, bool /*descending*/) const noexcept {
#if defined(__AVX2__)
    // AVX2 path: 4× int64_t per cycle (VPCMPEQQ ymm, ymm, ymm)
    // Scalar fallback handles tail elements when count % 4 != 0.
    return find_level_avx2(prices.data(), count, p);
#else
    // Scalar fallback for non-AVX2 targets
    for (uint32_t i = 0; i < count; ++i)
        if (prices[i] == p) return static_cast<int32_t>(i);
    return -1;
#endif
}


#if defined(__AVX2__)
int32_t find_level_avx2(
    const Price* __restrict__ prices,
    uint32_t count,
    Price target) noexcept
{
    const __m256i vtarget = _mm256_set1_epi64x(static_cast<long long>(target));
    uint32_t i = 0;
    // VPCMPEQQ ymm: 4 x int64_t per cycle, 1-cycle latency (Zen3/Ice Lake)
    for (; i + 4 <= count; i += 4) {
        __m256i chunk = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(prices + i));
        __m256i eq  = _mm256_cmpeq_epi64(chunk, vtarget);
        int     msk = _mm256_movemask_epi8(eq);
        if (msk) return static_cast<int32_t>(i + __builtin_ctz(msk) / 8);
    }
    for (; i < count; ++i)
        if (prices[i] == target) return static_cast<int32_t>(i);
    return -1;
}
#endif // __AVX2__

int32_t OrderBook::Side::insert_level(Price p, bool descending) noexcept {
    if (count >= kMaxLevels) return -1;

    uint32_t pos = 0;
    if (descending) {
        while (pos < count && prices[pos] > p) ++pos;
    } else {
        while (pos < count && prices[pos] < p) ++pos;
    }

    if (pos < count) {
        const uint32_t n = count - pos;
        std::memmove(&prices[pos+1],       &prices[pos],       n * sizeof(Price));
        std::memmove(&qtys[pos+1],         &qtys[pos],         n * sizeof(Qty));
        std::memmove(&order_counts[pos+1], &order_counts[pos], n * sizeof(uint32_t));
        std::memmove(&head_idxs[pos+1],    &head_idxs[pos],    n * sizeof(uint32_t));
        std::memmove(&tail_idxs[pos+1],    &tail_idxs[pos],    n * sizeof(uint32_t));
    }

    prices[pos]       = p;
    qtys[pos]         = 0;
    order_counts[pos] = 0;
    head_idxs[pos]    = NULL_IDX;
    tail_idxs[pos]    = NULL_IDX;
    ++count;
    return static_cast<int32_t>(pos);
}

void OrderBook::Side::remove_level(uint32_t idx) noexcept {
    assert(idx < count);
    const uint32_t n = count - idx - 1;
    if (n > 0) {
        std::memmove(&prices[idx],       &prices[idx+1],       n * sizeof(Price));
        std::memmove(&qtys[idx],         &qtys[idx+1],         n * sizeof(Qty));
        std::memmove(&order_counts[idx], &order_counts[idx+1], n * sizeof(uint32_t));
        std::memmove(&head_idxs[idx],    &head_idxs[idx+1],    n * sizeof(uint32_t));
        std::memmove(&tail_idxs[idx],    &tail_idxs[idx+1],    n * sizeof(uint32_t));
    }
    --count;
}

// ── OrderBook::SlotPool ───────────────────────────────────────────────────────

void OrderBook::SlotPool::init() noexcept {
    // Write every array once so each page is faulted in here, at
    // construction, not on the matching path the first time a slot is used.
    // Before this, only free_nexts was written: a fresh book took ~500 minor
    // page faults during its first 1M events (each a kernel entry) and ~0
    // after. The values are irrelevant — slot state is defined by the lists.
    ids.fill(0); qtys.fill(0); nexts.fill(NULL_IDX); prevs.fill(NULL_IDX);
    prices.fill(0); accounts.fill(0);
    for (uint32_t i = 0; i < kMaxOrders - 1; ++i)
        free_nexts[i] = i + 1;
    free_nexts[kMaxOrders - 1] = NULL_IDX;
    free_head = 0;
    used      = 0;
}

uint32_t OrderBook::SlotPool::alloc() noexcept {
    if (free_head == NULL_IDX) [[unlikely]] return NULL_IDX;
    uint32_t idx = free_head;
    free_head = free_nexts[idx];
    ++used;
    return idx;
}

void OrderBook::SlotPool::free_slot(uint32_t idx) noexcept {
    free_nexts[idx] = free_head;
    free_head  = idx;
    --used;
}

// ── OrderBook::OrderIndex ─────────────────────────────────────────────────────

void OrderBook::OrderIndex::init() noexcept {
    for (auto& e : table) { e.slot = kEmpty; e.key = 0; e.side = 0xFF; }
}

// Fibonacci hashing: multiply by 2^64/phi and keep the TOP kBits bits.
// The top bits of the product depend on every bit of the key, so sequential
// order IDs — how exchanges and this repo's own tests assign them — spread
// evenly across the table.
//
// An earlier version computed (id * 2654435761) >> 32, which for any id is
// about 0.618 * id: consecutive IDs landed in overlapping adjacent slots.
// With 65,536 sequential IDs live, linear probing then averaged ~12,500
// probes per lookup (max ~25,000) instead of ~1, and a cancel in an
// 8,000-order book took ~3.5 ms. See BENCHMARK_RESULTS.md, "Order index".
uint32_t OrderBook::OrderIndex::home(OrderId id) noexcept {
    return static_cast<uint32_t>((id * 0x9E3779B97F4A7C15ULL) >> (64 - kBits));
}

uint32_t OrderBook::OrderIndex::longest_cluster() const noexcept {
    // Start just after an empty slot so a run that wraps the end is counted
    // once, whole. (Load factor <= 0.5 guarantees an empty slot exists.)
    uint32_t start = 0;
    while (table[start].slot != kEmpty) ++start;
    uint32_t best = 0, run = 0;
    for (uint32_t n = 1; n <= kTableSize; ++n) {
        const uint32_t i = (start + n) & kMask;
        if (table[i].slot != kEmpty) { if (++run > best) best = run; }
        else run = 0;
    }
    return best;
}

uint32_t OrderBook::OrderIndex::probe(OrderId id) const noexcept {
    uint32_t h = home(id);
    while (table[h].slot != kEmpty && table[h].key != id)
        h = (h + 1) & kMask;
    return h;
}

bool OrderBook::OrderIndex::insert(OrderId id, uint32_t slot, uint8_t side) noexcept {
    uint32_t h = probe(id);
    if (table[h].slot != kEmpty && table[h].key != id) return false;
    table[h] = { id, slot, side };
    return true;
}

bool OrderBook::OrderIndex::lookup(OrderId id, uint32_t& slot_out, uint8_t& side_out) const noexcept {
    uint32_t h = probe(id);
    if (table[h].slot == kEmpty) return false;
    slot_out = table[h].slot;
    side_out = table[h].side;
    return true;
}

// Backward-shift deletion (linear probing): walk the cluster after the hole
// once; move an entry back into the hole unless its home slot lies
// cyclically in (hole, j], in which case moving it would place it before
// its own home and make it unreachable. O(cluster length), no tombstones.
//
// An earlier version removed every following entry and re-probed each one
// from scratch — correct, but O(cluster^2), which compounded the hash
// problem above.
bool OrderBook::OrderIndex::remove(OrderId id) noexcept {
    uint32_t hole = probe(id);
    if (table[hole].slot == kEmpty) return false;
    uint32_t j = hole;
    for (;;) {
        j = (j + 1) & kMask;
        if (table[j].slot == kEmpty) break;
        const uint32_t h = home(table[j].key);
        const bool stays = (hole <= j) ? (hole < h && h <= j)
                                       : (hole < h || h <= j);
        if (stays) continue;
        table[hole] = table[j];
        hole = j;
    }
    table[hole] = Entry{};
    return true;
}

// ── OrderBook ─────────────────────────────────────────────────────────────────

OrderBook::OrderBook(SymbolId symbol, MatchCallback on_match)
    : match_cb_(std::move(on_match)), symbol_(symbol)
{
    slots_.init();
    index_.init();
    // Price-level arrays: same reason — touch every page up front.
    for (Side* sd : {&bids_, &asks_}) {
        sd->prices.fill(0); sd->qtys.fill(0); sd->order_counts.fill(0);
        sd->head_idxs.fill(NULL_IDX); sd->tail_idxs.fill(NULL_IDX);
    }
}

bool OrderBook::add_order(const Order& order) noexcept {
    ++stat_orders_;
    Order o = order;

    if (o.type == OrderType::Market || o.type == OrderType::IOC || o.type == OrderType::FOK) {
        // FOK is all-or-none and atomic: check liquidity BEFORE executing so a
        // rejected FOK never leaves partial fills behind.
        if (o.type == OrderType::FOK && available_liquidity(o) < o.qty_remaining) {
            ++stat_rejected_;
            return false;
        }
        try_match(o);
        // Market, IOC, and (now fully filled) FOK never rest. Any remainder
        // is expired, not booked.
        return true;
    }
    bool stp_walled = try_match(o);

    if (o.is_filled()) return true;

    if (stp_walled) {
        // Refused to match through this price because the resting order
        // there is ours (self-trade prevention). Resting the remainder
        // anyway would leave the book crossed against that price, so the
        // remainder is cancelled instead of booked — same externally
        // visible outcome as an IOC's unfilled remainder. See
        // LIMITATIONS.md.
        ++stat_self_trade_prevented_;
        return true;
    }

    bool is_bid = o.is_buy();
    Side& side  = is_bid ? bids_ : asks_;

    int32_t lvl_idx = side.find_level(o.price, is_bid);
    bool created_level = false;
    if (lvl_idx < 0) {
        lvl_idx = side.insert_level(o.price, is_bid);
        if (lvl_idx < 0) { ++stat_rejected_; return false; }
        created_level = true;
    }

    uint32_t slot = slots_.alloc();
    if (slot == NULL_IDX) {
        // Roll back the level we just created — otherwise a capacity
        // rejection here leaves a permanent empty price level behind
        // (order_count=0, qty=0) that pollutes best_quote()/find_level()
        // and silently eats one of the kMaxLevels slots. Only roll back
        // when we created it: an existing level with other resting
        // orders must never be touched by this failure path.
        if (created_level) side.remove_level(static_cast<uint32_t>(lvl_idx));
        ++stat_rejected_;
        return false;
    }

    slots_.ids[slot]        = o.id;
    slots_.accounts[slot]   = o.account_id;
    slots_.qtys[slot]       = o.qty_remaining;
    slots_.nexts[slot]      = NULL_IDX;
    // Store price instead of index — index shifts on remove_level.
    slots_.prices[slot]     = o.price;

    // O(1) append using tail_idxs — no traversal needed.
    // Maintain doubly-linked list: set prev of new slot = old tail,
    // then advance tail. The old head path sets prev = NULL_IDX.
    if (side.head_idxs[lvl_idx] == NULL_IDX) {
        // First order at this level.
        slots_.prevs[slot]      = NULL_IDX;
        side.head_idxs[lvl_idx] = slot;
        side.tail_idxs[lvl_idx] = slot;
    } else {
        // Link new slot after current tail, then advance tail pointer.
        const uint32_t old_tail = side.tail_idxs[lvl_idx];
        slots_.nexts[old_tail]  = slot;
        slots_.prevs[slot]      = old_tail;
        side.tail_idxs[lvl_idx] = slot;
    }

    side.qtys[lvl_idx]         += o.qty_remaining;
    side.order_counts[lvl_idx] += 1;

    index_.insert(o.id, slot, is_bid ? 0 : 1);
    return true;
}

bool OrderBook::cancel_order(OrderId order_id) noexcept {
    uint32_t slot; uint8_t side_idx;
    if (!index_.lookup(order_id, slot, side_idx)) return false;

    Side& side   = (side_idx == 0) ? bids_ : asks_;
    Price price  = slots_.prices[slot];
    int32_t lvl  = side.find_level(price, side_idx == 0);
    if (lvl < 0) {
        // Level gone (fully matched) — just clean up index.
        index_.remove(order_id);
        return false;
    }

    remove_order_from_level(side_idx, static_cast<uint32_t>(lvl), slot);
    index_.remove(order_id);
    ++stat_cancelled_;
    return true;
}

bool OrderBook::modify_order(OrderId order_id, Qty new_qty) noexcept {
    uint32_t slot; uint8_t side_idx;
    if (!index_.lookup(order_id, slot, side_idx)) return false;

    Side& side  = (side_idx == 0) ? bids_ : asks_;
    Price price = slots_.prices[slot];
    int32_t lvl = side.find_level(price, side_idx == 0);
    if (lvl < 0) { index_.remove(order_id); return false; }

    const Qty old_qty = slots_.qtys[slot];
    const uint32_t lvl_u = static_cast<uint32_t>(lvl);

    if (new_qty > old_qty) {
        // Quantity increase loses time priority: unlink from the current
        // spot in the level's FIFO and re-append at the tail, exactly as if
        // it were a brand-new order arriving at this price. Mirrors the
        // append logic in add_order() so the two stay in sync.
        {
            const uint32_t prev = slots_.prevs[slot];
            const uint32_t next = slots_.nexts[slot];
            if (prev != NULL_IDX) slots_.nexts[prev] = next;
            else                  side.head_idxs[lvl_u] = next;
            if (next != NULL_IDX) slots_.prevs[next] = prev;
            else                  side.tail_idxs[lvl_u] = prev;
        }
        if (side.head_idxs[lvl_u] == NULL_IDX) {
            slots_.prevs[slot]    = NULL_IDX;
            slots_.nexts[slot]    = NULL_IDX;
            side.head_idxs[lvl_u] = slot;
            side.tail_idxs[lvl_u] = slot;
        } else {
            const uint32_t old_tail = side.tail_idxs[lvl_u];
            slots_.nexts[old_tail] = slot;
            slots_.prevs[slot]     = old_tail;
            slots_.nexts[slot]     = NULL_IDX;
            side.tail_idxs[lvl_u]  = slot;
        }
        side.qtys[lvl_u]  += (new_qty - old_qty);
        slots_.qtys[slot]  = new_qty;
    } else if (new_qty < old_qty) {
        // Decrease keeps queue position — correct per price-time priority.
        side.qtys[lvl_u]  -= (old_qty - new_qty);
        slots_.qtys[slot]  = new_qty;
        if (new_qty == 0) {
            remove_order_from_level(side_idx, lvl_u, slot);
            index_.remove(order_id);
        }
    }
    // new_qty == old_qty: no-op, priority unaffected.
    return true;
}

Qty OrderBook::available_liquidity(const Order& incoming) const noexcept {
    const bool  buy  = incoming.is_buy();
    const Side& side = buy ? asks_ : bids_;
    uint64_t total = 0;
    for (uint32_t i = 0; i < side.count; ++i) {
        const Price px = side.prices[i];
        if (incoming.type != OrderType::Market) {
            if (buy  && incoming.price < px) break;
            if (!buy && incoming.price > px) break;
        }
        // Walk this level's queue rather than summing side.qtys[i] in one
        // shot, so a same-account order anywhere in the queue (an STP
        // wall — see try_match) is accounted for exactly, not just one at
        // a level's head. This is what keeps the FOK atomicity guarantee
        // under STP: a FOK that passes this pre-check is guaranteed to
        // fully fill in try_match, wall or no wall. Degrades to the same
        // O(1)-per-level cost as before whenever account_id is 0
        // (untracked) on either side, which is every existing caller.
        uint32_t idx = side.head_idxs[i];
        while (idx != NULL_IDX) {
            if (incoming.account_id != 0 && incoming.account_id == slots_.accounts[idx])
                return Qty(std::min<uint64_t>(total, UINT32_MAX));
            total += slots_.qtys[idx];
            if (total >= incoming.qty_remaining)
                return Qty(std::min<uint64_t>(total, UINT32_MAX));
            idx = slots_.nexts[idx];
        }
    }
    return Qty(std::min<uint64_t>(total, UINT32_MAX));
}

bool OrderBook::try_match(Order& incoming) noexcept {
    bool aggressor_is_buy = incoming.is_buy();
    Side& passive_side    = aggressor_is_buy ? asks_ : bids_;
    bool stp_walled = false;

    while (incoming.qty_remaining > 0 && passive_side.count > 0) {
        Price best_passive = passive_side.prices[0];

        // IOC and FOK are limit orders with a time-in-force; only a true
        // Market order ignores its price.
        if (incoming.type != OrderType::Market) {
            if (aggressor_is_buy  && incoming.price < best_passive) break;
            if (!aggressor_is_buy && incoming.price > best_passive) break;
        }

        // Always work level index 0 (best).
        while (passive_side.head_idxs[0] != NULL_IDX && incoming.qty_remaining > 0) {
            uint32_t slot = passive_side.head_idxs[0];

            // Self-trade prevention: if the head-of-queue passive order
            // belongs to the same (non-zero) account as the aggressor,
            // stop matching entirely rather than skip past it — skipping
            // would match a later-queued order first, violating FIFO for
            // everyone behind it. The caller must NOT rest the aggressor's
            // remainder in this case: since we refused to match through
            // this price, resting here would leave the book crossed. See
            // add_order() and LIMITATIONS.md.
            if (incoming.account_id != 0 && incoming.account_id == slots_.accounts[slot]) {
                return true;
            }

            const Qty fill_qty = std::min(incoming.qty_remaining, slots_.qtys[slot]);

            // Update quantities.
            slots_.qtys[slot]       -= fill_qty;
            passive_side.qtys[0]    -= fill_qty;
            incoming.qty_remaining  -= fill_qty;

            // Fire execution report.
            if (match_cb_) {
                ExecutionReport rpt{};
                rpt.order_id        = incoming.id;
                rpt.contra_order_id = slots_.ids[slot];
                rpt.exec_price      = best_passive;
                rpt.exec_qty        = fill_qty;
                rpt.leaves_qty      = incoming.qty_remaining;
                rpt.symbol          = symbol_;
                rpt.side            = incoming.side;
                rpt.exec_type       = (incoming.qty_remaining == 0)
                                      ? ExecType::Fill : ExecType::PartialFill;
                match_cb_(rpt);
            }
            ++stat_matches_;

            if (slots_.qtys[slot] == 0) {
                // Passive order fully filled — dequeue from head and free.
                passive_side.head_idxs[0] = slots_.nexts[slot];
                // If the level is now empty, reset tail too.
                if (passive_side.head_idxs[0] == NULL_IDX)
                    passive_side.tail_idxs[0] = NULL_IDX;
                else
                    slots_.prevs[passive_side.head_idxs[0]] = NULL_IDX;
                passive_side.order_counts[0] -= 1;
                index_.remove(slots_.ids[slot]);
                slots_.free_slot(slot);
            }
        }

        // Remove exhausted level.
        if (passive_side.order_counts[0] == 0)
            passive_side.remove_level(0);
    }
    return stp_walled;
}

void OrderBook::remove_order_from_level(uint8_t side_idx, uint32_t level_idx,
                                         uint32_t slot) noexcept
{
    Side& side = (side_idx == 0) ? bids_ : asks_;

    side.qtys[level_idx]         -= slots_.qtys[slot];
    side.order_counts[level_idx] -= 1;

    // O(1) unlink — doubly-linked intrusive list via slots_.prevs[].
    {
        const uint32_t prev = slots_.prevs[slot];
        const uint32_t next = slots_.nexts[slot];
        if (prev != NULL_IDX) slots_.nexts[prev] = next;
        else                  side.head_idxs[level_idx] = next;
        if (next != NULL_IDX) slots_.prevs[next] = prev;
        else                  side.tail_idxs[level_idx] = prev;
    }

    slots_.free_slot(slot);

    if (side.order_counts[level_idx] == 0)
        side.remove_level(level_idx);
}

BestQuote OrderBook::best_quote() const noexcept {
    BestQuote q{};
    q.symbol    = symbol_;
    q.bid_price = bids_.count > 0 ? bids_.prices[0] : PRICE_INVALID;
    q.ask_price = asks_.count > 0 ? asks_.prices[0] : PRICE_INVALID;
    q.bid_qty   = bids_.count > 0 ? bids_.qtys[0]   : 0;
    q.ask_qty   = asks_.count > 0 ? asks_.qtys[0]   : 0;
    return q;
}

std::size_t OrderBook::bid_depth(std::span<PriceLevel> out) const noexcept {
    std::size_t n = std::min(out.size(), static_cast<std::size_t>(bids_.count));
    for (std::size_t i = 0; i < n; ++i)
        out[i] = { bids_.prices[i], bids_.qtys[i], bids_.order_counts[i], bids_.head_idxs[i] };
    return n;
}

std::size_t OrderBook::ask_depth(std::span<PriceLevel> out) const noexcept {
    std::size_t n = std::min(out.size(), static_cast<std::size_t>(asks_.count));
    for (std::size_t i = 0; i < n; ++i)
        out[i] = { asks_.prices[i], asks_.qtys[i], asks_.order_counts[i], asks_.head_idxs[i] };
    return n;
}

} // namespace engine
