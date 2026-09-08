#pragma once
// Feed boundary: anything that produces engine::MarketDataMsg.
// Production: adapt udp-multicast-receiver / ITCH into this shape.
// Demo: SyntheticFeed + PcapStyleReplay (deterministic event stream).

#include "core/types.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace hft {

using FeedSink = std::function<bool(const engine::MarketDataMsg&)>;

struct SyntheticFeedConfig {
    engine::SymbolId symbol{1};
    engine::Price    mid{engine::to_price(100.0)};
    uint64_t         n_events{10000};
    uint32_t         seed{1};
};

// Deterministic alternating buy/sell cluster around mid so crosses occur.
inline std::vector<engine::MarketDataMsg>
make_synthetic_stream(const SyntheticFeedConfig& cfg) {
    std::vector<engine::MarketDataMsg> out;
    out.reserve(cfg.n_events);
    engine::OrderId id = 1;
    for (uint64_t i = 0; i < cfg.n_events; ++i) {
        engine::MarketDataMsg m{};
        m.seq = i + 1;
        m.order_id = id++;
        m.symbol = cfg.symbol;
        m.side = (i % 2 == 0) ? engine::Side::Buy : engine::Side::Sell;
        m.msg_type = engine::MarketDataMsg::Type::NewOrder;
        m.order_type = engine::OrderType::Limit;
        const int offset = static_cast<int>((i * 17 + cfg.seed) % 21) - 10;
        m.price = cfg.mid + static_cast<engine::Price>(offset * 1000);
        m.qty = 10 + static_cast<engine::Qty>((i + cfg.seed) % 40);
        out.push_back(m);
    }
    return out;
}

// Drive a pre-built stream into a sink (pipeline submit). Returns accepted count.
inline uint64_t replay_stream(const std::vector<engine::MarketDataMsg>& stream,
                              const FeedSink& sink) {
    uint64_t accepted = 0;
    for (const auto& m : stream) {
        if (sink(m)) ++accepted;
    }
    return accepted;
}

} // namespace hft
