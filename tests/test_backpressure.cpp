#include "core/matching_engine.hpp"
#include "core/types.hpp"
#include "hft/check.hpp"
#include <cstdio>

int main() {
    engine::MatchingEngine eng;
    CHECK(eng.register_symbol(1), "register_symbol");

    engine::MarketDataMsg m{};
    m.symbol = 1;
    m.msg_type = engine::MarketDataMsg::Type::NewOrder;
    m.order_type = engine::OrderType::Limit;
    m.side = engine::Side::Buy;
    m.price = engine::to_price(100.0);
    m.qty = 1;

    uint64_t accepted = 0;
    uint64_t rejected = 0;
    for (uint64_t i = 0; i < 70000; ++i) {
        m.seq = i + 1;
        m.order_id = i + 1;
        if (eng.submit(m)) ++accepted;
        else ++rejected;
    }
    std::printf("backpressure: accepted=%llu rejected=%llu\n",
                (unsigned long long)accepted, (unsigned long long)rejected);

    CHECK(accepted > 0, "some messages accepted");
    CHECK(rejected > 0, "queue full must reject under burst without consumer");
    // Engine inbound depth is 65536; full leaves one slot free in classic SPSC
    CHECK(accepted <= 65536, "accepted cannot exceed queue capacity");
    CHECK(rejected >= 4000, "enough rejects to prove back-pressure");

    TEST_EXIT();
}
