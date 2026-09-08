#pragma once
#include "core/types.hpp"
#include "feed/wire_format.hpp"
#include "feed/gap_buffer.hpp"
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

class ItchAdapter {
public:
    explicit ItchAdapter(ItchAdapterConfig cfg, MdHandler on_md)
        : cfg_(cfg), on_md_(std::move(on_md)) {}

    void on_mold_message(const feed::MoldMessage& msg) {
        if (!msg.body || msg.length == 0) return;
        const auto* body = msg.body;
        const auto t = feed::itch_type(body);
        engine::MarketDataMsg m{};
        m.seq = msg.seq_num;
        m.symbol = cfg_.symbol;
        const uint64_t recv_ns = msg.recv_ns;

        switch (t) {
        case feed::ItchMsgType::AddOrderNoMpid:
        case feed::ItchMsgType::AddOrderMpid: {
            feed::ItchAddOrder add{};
            if (!feed::ItchAddOrder::parse(body, msg.length, add)) return;
            m.msg_type = engine::MarketDataMsg::Type::NewOrder;
            m.order_type = engine::OrderType::Limit;
            m.order_id = add.order_ref;
            m.price = ItchAdapterConfig::itch_price_to_engine(add.price);
            m.qty = add.shares;
            m.side = (add.side == 'B') ? engine::Side::Buy : engine::Side::Sell;
            on_md_(m, recv_ns);
            break;
        }
        case feed::ItchMsgType::OrderDelete: {
            feed::ItchDeleteOrder del{};
            if (!feed::ItchDeleteOrder::parse(body, msg.length, del)) return;
            m.msg_type = engine::MarketDataMsg::Type::CancelOrder;
            m.order_id = del.order_ref;
            on_md_(m, recv_ns);
            break;
        }
        case feed::ItchMsgType::OrderCancel: {
            feed::ItchOrderCancel can{};
            if (!feed::ItchOrderCancel::parse(body, msg.length, can)) return;
            m.msg_type = engine::MarketDataMsg::Type::CancelOrder;
            m.order_id = can.order_ref;
            m.qty = can.cancelled_shares;
            on_md_(m, recv_ns);
            break;
        }
        default: break;
        }
    }

    void attach(feed::GapBuffer& gb) {
        gb.set_on_message([this](const feed::MoldMessage& msg) { on_mold_message(msg); });
    }

private:
    ItchAdapterConfig cfg_;
    MdHandler on_md_;
};

inline std::vector<uint8_t> make_mold_add_packet(uint64_t seq, uint64_t order_ref,
                                                   char side, uint32_t shares,
                                                   uint32_t itch_price) {
    const uint16_t itch_len = 36;
    std::vector<uint8_t> itch(itch_len, 0);
    itch[0] = static_cast<uint8_t>('A');
    uint64_t be_ref = __builtin_bswap64(order_ref);
    std::memcpy(itch.data() + 7, &be_ref, 8);
    itch[15] = static_cast<uint8_t>(side);
    uint32_t be_sh = __builtin_bswap32(shares);
    std::memcpy(itch.data() + 16, &be_sh, 4);
    std::memcpy(itch.data() + 20, "TEST     ", 8);
    uint32_t be_px = __builtin_bswap32(itch_price);
    std::memcpy(itch.data() + 28, &be_px, 4);

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
