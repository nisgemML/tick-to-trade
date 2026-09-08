#include "hft/check.hpp"
#include "hft/itch_adapter.hpp"
#include "feed/gap_buffer.hpp"
#include <cstdio>
#include <memory>
#include <vector>

// Proves GapBuffer gap path is exercised: skip seq 2, deliver 1 then 3,
// then fill the gap with 2 — on_gap must fire and messages eventually deliver.

int main() {
    std::vector<engine::MarketDataMsg> out;
    int gaps = 0;

    auto gb = std::make_unique<feed::GapBuffer>();
    gb->set_on_gap([&](const feed::GapEvent&) { ++gaps; });

    hft::ItchAdapter ad({1}, [&](const engine::MarketDataMsg& m, uint64_t) {
        out.push_back(m);
    });
    ad.attach(*gb);

    // seq 1 in order
    auto p1 = hft::make_mold_add_packet(1, 1, 'B', 10, 1000000);
    CHECK(gb->ingest(p1.data(), p1.size(), 100) >= 0, "ingest seq1");
    CHECK(out.size() == 1, "seq1 delivered");

    // seq 3 arrives first → gap (missing 2)
    auto p3 = hft::make_mold_add_packet(3, 3, 'S', 10, 1000100);
    (void)gb->ingest(p3.data(), p3.size(), 200);
    CHECK(gaps >= 1, "gap detected when seq3 arrives before seq2");
    CHECK(out.size() == 1, "seq3 held until gap filled");

    // seq 2 fills gap → should deliver 2 then buffered 3
    auto p2 = hft::make_mold_add_packet(2, 2, 'B', 10, 1000050);
    (void)gb->ingest(p2.data(), p2.size(), 300);
    CHECK(out.size() == 3, "seq2 and buffered seq3 delivered after gap fill");
    CHECK(out[1].order_id == 2, "order 2");
    CHECK(out[2].order_id == 3, "order 3");

    // duplicate seq1 should not add another message
    (void)gb->ingest(p1.data(), p1.size(), 400);
    CHECK(out.size() == 3, "duplicate seq1 dropped");

    std::printf("gap_injection: gaps=%d delivered=%zu\n", gaps, out.size());
    TEST_EXIT();
}
