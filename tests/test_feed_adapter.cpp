#include "hft/itch_adapter.hpp"
#include <cassert>
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
    assert(gb->ingest(pkt.data(), pkt.size(), 1000) >= 0);
    assert(out.size() == 1);
    assert(out[0].order_id == 42 && out[0].symbol == 7);
    std::printf("test_feed_adapter OK\n");
    return 0;
}
