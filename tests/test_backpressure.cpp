#include "core/matching_engine.hpp"
#include "core/types.hpp"
#include <cassert>
#include <cstdio>

// Failure mode: inbound queue eventually rejects under extreme burst
// without a consumer draining (engine not started).
int main() {
    engine::MatchingEngine eng;
    assert(eng.register_symbol(1));

    engine::MarketDataMsg m{};
    m.symbol = 1;
    m.msg_type = engine::MarketDataMsg::Type::NewOrder;
    m.order_type = engine::OrderType::Limit;
    m.side = engine::Side::Buy;
    m.price = engine::to_price(100.0);
    m.qty = 1;

    uint64_t accepted = 0;
    uint64_t rejected = 0;
    // Queue depth is 65536; push more without starting the engine.
    for (uint64_t i = 0; i < 70000; ++i) {
        m.seq = i + 1;
        m.order_id = i + 1;
        if (eng.submit(m)) ++accepted;
        else ++rejected;
    }
    std::printf("backpressure: accepted=%llu rejected=%llu\n",
                (unsigned long long)accepted, (unsigned long long)rejected);
    assert(rejected > 0);
    assert(accepted > 0);
    std::printf("test_backpressure OK\n");
    return 0;
}
