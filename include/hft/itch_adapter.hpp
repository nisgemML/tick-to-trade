#pragma once
#include "core/types.hpp"
#include "feed/wire_format.hpp"
#include "feed/gap_buffer.hpp"
#include "hft/itch50.hpp"
#include <cstdint>
#include <functional>
#include <vector>
#include <cstring>

namespace hft {

using MdHandler = std::function<void(const engine::MarketDataMsg&, uint64_t recv_ns)>;

struct ItchAdapterConfig {
    engine::SymbolId symbol{1};
    static engine::Price itch_price_to_engine(uint32_t itch_px) {
        return static_cast<engine::Price>(itch_px) * 100;
    }
};

// Maps the live MoldUDP64 feed onto the matching engine's message model.
//
// Decoding and the ITCH -> engine mapping are delegated to hft::itch50
// (spec-accurate decoders + Translator), the same code the real-data replay
// path uses and the one checked against the independent itchfeed oracle.
// Before this, the adapter had its own mapping that turned a partial cancel
// ('X') into a full cancel and dropped executions ('E', 'C') and replaces
// ('U') — BUGS_FOUND.md #16. One mapping, one set of tests.
//
// Translator keeps per-order state (side, price, remaining shares), which a
// correct mapping needs: 'X'/'E'/'C' carry only an order ref and a share
// delta. That state is an unordered_map on this path; see LIMITATIONS.md.
class ItchAdapter {
public:
    explicit ItchAdapter(ItchAdapterConfig cfg, MdHandler on_md)
        : cfg_(cfg), on_md_(std::move(on_md)), tr_(cfg.symbol) {}

    void on_mold_message(const feed::MoldMessage& msg) {
        if (!msg.body || msg.length == 0) return;
        const uint8_t* b = msg.body;
        const std::size_t n = msg.length;
        itch50::Translator::Out out;

        switch (b[0]) {
        case 'A': case 'F': {
            itch50::AddOrder o;
            if (!itch50::AddOrder::parse(b, n, o)) { ++parse_errors_; return; }
            out = tr_.on_add(o.order_ref, o.side, o.price, o.shares);
            break;
        }
        case 'E': case 'C': {
            itch50::OrderExecuted o;
            if (!itch50::OrderExecuted::parse(b, n, o)) { ++parse_errors_; return; }
            out = tr_.on_reduce(o.order_ref, o.shares, /*is_exec=*/true);
            break;
        }
        case 'X': {
            itch50::OrderCancel o;
            if (!itch50::OrderCancel::parse(b, n, o)) { ++parse_errors_; return; }
            out = tr_.on_reduce(o.order_ref, o.canceled_shares, /*is_exec=*/false);
            break;
        }
        case 'D': {
            itch50::OrderDelete o;
            if (!itch50::OrderDelete::parse(b, n, o)) { ++parse_errors_; return; }
            out = tr_.on_delete(o.order_ref);
            break;
        }
        case 'U': {
            itch50::OrderReplace o;
            if (!itch50::OrderReplace::parse(b, n, o)) { ++parse_errors_; return; }
            out = tr_.on_replace(o.orig_ref, o.new_ref, o.shares, o.price);
            break;
        }
        default: return;  // non-book messages (system, directory, trades...)
        }

        for (int i = 0; i < out.n; ++i) {
            out.msgs[i].seq = msg.seq_num;  // carry the feed sequence through
            on_md_(out.msgs[i], msg.recv_ns);
        }
    }

    void attach(feed::GapBuffer& gb) {
        gb.set_on_message([this](const feed::MoldMessage& msg) { on_mold_message(msg); });
    }

    [[nodiscard]] const itch50::Translator::Counters& counters() const { return tr_.counters(); }
    [[nodiscard]] uint64_t parse_errors() const { return parse_errors_; }

private:
    ItchAdapterConfig cfg_;
    MdHandler on_md_;
    itch50::Translator tr_;
    uint64_t parse_errors_{0};
};

inline std::vector<uint8_t> make_mold_add_packet(uint64_t seq, uint64_t order_ref,
                                                   char side, uint32_t shares,
                                                   uint32_t itch_price) {
    const uint16_t itch_len = 36;
    std::vector<uint8_t> itch(itch_len, 0);
    itch[0] = static_cast<uint8_t>('A');
    uint64_t be_ref = __builtin_bswap64(order_ref);
    std::memcpy(itch.data() + 11, &be_ref, 8);
    itch[19] = static_cast<uint8_t>(side);
    uint32_t be_sh = __builtin_bswap32(shares);
    std::memcpy(itch.data() + 20, &be_sh, 4);
    std::memcpy(itch.data() + 24, "TEST     ", 8);
    uint32_t be_px = __builtin_bswap32(itch_price);
    std::memcpy(itch.data() + 32, &be_px, 4);

    std::vector<uint8_t> pkt(20 + 2 + itch_len, 0);
    std::memset(pkt.data(), ' ', 10);
    uint64_t be_seq = __builtin_bswap64(seq);
    std::memcpy(pkt.data() + 10, &be_seq, 8);
    uint16_t be_cnt = __builtin_bswap16(1);
    std::memcpy(pkt.data() + 18, &be_cnt, 2);
    uint16_t be_mlen = __builtin_bswap16(itch_len);
    std::memcpy(pkt.data() + 20, &be_mlen, 2);
    std::memcpy(pkt.data() + 22, itch.data(), itch_len);
    return pkt;
}

} // namespace hft
