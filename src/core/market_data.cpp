#include "core/market_data.hpp"
#include <cstring>

namespace engine {

static constexpr uint16_t kMsgNewOrder    = 1;
static constexpr uint16_t kMsgCancelOrder = 2;
static constexpr uint16_t kMsgModify      = 3;
static constexpr uint16_t kMsgHeartbeat   = 99;

int MarketDataIngestion::ingest(std::span<const uint8_t> data) noexcept {
    if (data.size() < sizeof(WireHeader)) {
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    WireHeader hdr;
    std::memcpy(&hdr, data.data(), sizeof(hdr));

    if (hdr.magic != kMagic) {
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Sequence number gap detection.
    uint64_t expected = expected_seq_.load(std::memory_order_relaxed);
    if (expected != 0 && hdr.seq_num != expected) {
        stat_gaps_.fetch_add(1, std::memory_order_relaxed);
        if (gap_cb_) gap_cb_(expected, hdr.seq_num);
    }
    expected_seq_.store(hdr.seq_num + 1, std::memory_order_relaxed);

    // Validate the declared payload length against what's ACTUALLY in
    // `data` before constructing a subspan from it — not after. Calling
    // std::span::subspan(offset, count) with count > size() - offset is
    // undefined behavior per the standard: libstdc++'s implementation
    // does not clamp or validate it, it just stores whatever count was
    // requested as the resulting span's size. That means the check this
    // function used to run AFTER the subspan call —
    // `if (payload.size() < hdr.payload_len)` — was comparing
    // hdr.payload_len against itself and could never be true: `payload`
    // was already forced to report exactly hdr.payload_len as its size,
    // whether or not `data` actually had that many bytes. The
    // std::memcpy() calls in decode_new_order()/decode_cancel()/
    // decode_modify() then read straight past the end of the real
    // buffer. Confirmed as a genuine, exploitable heap-buffer-overflow
    // READ under AddressSanitizer (a caller passing a byte buffer sized
    // to exactly the bytes actually received — e.g. a UDP datagram — and
    // a wire payload_len field larger than that): "AddressSanitizer:
    // unknown-crash ... in memcpy", not merely a logic bug that happened
    // to decode garbage. A malformed or truncated feed message is
    // untrusted, adversary-influenced input for this class by design —
    // this bound must be checked against the real buffer, not inferred
    // from a span that was already told to lie about its own size.
    if (data.size() < sizeof(WireHeader) + std::size_t(hdr.payload_len)) {
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    auto payload = data.subspan(sizeof(WireHeader), hdr.payload_len);

    MarketDataMsg msg{};
    msg.seq = hdr.seq_num;
    bool ok = false;

    switch (hdr.msg_type) {
        case kMsgNewOrder:
            ok = decode_new_order(hdr, payload, msg);
            break;
        case kMsgCancelOrder:
            ok = decode_cancel(hdr, payload, msg);
            break;
        case kMsgModify:
            ok = decode_modify(hdr, payload, msg);
            break;
        case kMsgHeartbeat:
            msg.msg_type = MarketDataMsg::Type::Heartbeat;
            ok = true;
            break;
        default:
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
            return 0;
    }

    if (!ok) {
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    if (!outbound_.try_push(msg)) {
        // Queue full — in production, trigger back-pressure or alert.
        return 0;
    }

    stat_ingested_.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

bool MarketDataIngestion::decode_new_order(const WireHeader&,
                                            std::span<const uint8_t> payload,
                                            MarketDataMsg& out) const noexcept
{
    if (payload.size() < sizeof(WireNewOrder)) return false;
    WireNewOrder w;
    std::memcpy(&w, payload.data(), sizeof(w));

    out.msg_type   = MarketDataMsg::Type::NewOrder;
    out.order_id   = w.order_id;
    out.price      = static_cast<Price>(w.price_fp);
    out.qty        = w.qty;
    out.symbol     = w.symbol_id;
    out.side       = (w.side == 0) ? Side::Buy : Side::Sell;
    out.order_type = static_cast<OrderType>(w.order_type);
    return true;
}

bool MarketDataIngestion::decode_cancel(const WireHeader&,
                                         std::span<const uint8_t> payload,
                                         MarketDataMsg& out) const noexcept
{
    if (payload.size() < sizeof(WireCancelOrder)) return false;
    WireCancelOrder w;
    std::memcpy(&w, payload.data(), sizeof(w));

    out.msg_type = MarketDataMsg::Type::CancelOrder;
    out.order_id = w.order_id;
    out.symbol   = w.symbol_id;
    return true;
}

bool MarketDataIngestion::decode_modify(const WireHeader&,
                                         std::span<const uint8_t> payload,
                                         MarketDataMsg& out) const noexcept
{
    if (payload.size() < sizeof(WireModifyOrder)) return false;
    WireModifyOrder w;
    std::memcpy(&w, payload.data(), sizeof(w));

    out.msg_type = MarketDataMsg::Type::ModifyOrder;
    out.order_id = w.order_id;
    out.qty      = w.new_qty;
    out.symbol   = w.symbol_id;
    return true;
}

} // namespace engine
