#include "hft/check.hpp"
#include "hft/itch_adapter.hpp"
#include <cstdio>
#include <memory>
#include <vector>

int main() {
    std::vector<engine::MarketDataMsg> out;
    auto gb = std::make_unique<feed::GapBuffer>();
    hft::ItchAdapter ad({7}, [&](const engine::MarketDataMsg& m, uint64_t) {
        out.push_back(m);
    });
    ad.attach(*gb);

    auto pkt = hft::make_mold_add_packet(1, 42, 'B', 100, 1234567);
    const int n = gb->ingest(pkt.data(), pkt.size(), 1000);
    std::printf("ingest returned %d out_size=%zu\n", n, out.size());

    CHECK(n >= 0, "ingest succeeds");
    CHECK(out.size() == 1, "one MarketDataMsg");
    CHECK(out[0].order_id == 42, "order_ref mapped");
    CHECK(out[0].symbol == 7, "symbol mapped");
    CHECK(out[0].side == engine::Side::Buy, "side Buy");
    CHECK(out[0].qty == 100, "qty");
    CHECK(out[0].msg_type == engine::MarketDataMsg::Type::NewOrder, "NewOrder");
    // ITCH ×1e4 -> engine ×1e6 via ×100
    CHECK(out[0].price == engine::Price(1234567) * 100, "price scale ×100");

    TEST_EXIT();
}
