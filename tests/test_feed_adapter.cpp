#include "hft/check.hpp"
#include "hft/itch_adapter.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

// Spec-layout ITCH bodies (type | locate(2) | tracking(2) | ts(6) | fields@11),
// wrapped in a one-message MoldUDP64 packet.
static std::vector<uint8_t> mold(uint64_t seq, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> p(22 + body.size(), 0);
    std::memset(p.data(), ' ', 10);
    feed::put_be64(p.data() + 10, seq);
    feed::put_be16(p.data() + 18, 1);
    feed::put_be16(p.data() + 20, uint16_t(body.size()));
    std::memcpy(p.data() + 22, body.data(), body.size());
    return p;
}
static std::vector<uint8_t> body_x(uint64_t ref, uint32_t sh) { std::vector<uint8_t> b(23, 0); b[0] = 'X'; feed::put_be64(b.data() + 11, ref); feed::put_be32(b.data() + 19, sh); return b; }
static std::vector<uint8_t> body_e(uint64_t ref, uint32_t sh) { std::vector<uint8_t> b(31, 0); b[0] = 'E'; feed::put_be64(b.data() + 11, ref); feed::put_be32(b.data() + 19, sh); return b; }
static std::vector<uint8_t> body_d(uint64_t ref)              { std::vector<uint8_t> b(19, 0); b[0] = 'D'; feed::put_be64(b.data() + 11, ref); return b; }
static std::vector<uint8_t> body_u(uint64_t o, uint64_t n, uint32_t sh, uint32_t px) {
    std::vector<uint8_t> b(35, 0); b[0] = 'U'; feed::put_be64(b.data() + 11, o); feed::put_be64(b.data() + 19, n);
    feed::put_be32(b.data() + 27, sh); feed::put_be32(b.data() + 31, px); return b;
}

// BUGS_FOUND.md #16: the live adapter used to turn a partial cancel into a full
// cancel and drop executions and replaces. Drive each through the real GapBuffer.
static void test_reductions_and_replace() {
    using T = engine::MarketDataMsg::Type;
    std::vector<engine::MarketDataMsg> out;
    auto gb = std::make_unique<feed::GapBuffer>();
    hft::ItchAdapter ad({7}, [&](const engine::MarketDataMsg& m, uint64_t) { out.push_back(m); });
    ad.attach(*gb);
    uint64_t seq = 1;
    auto feedp = [&](const std::vector<uint8_t>& p) { (void)gb->ingest(p.data(), p.size(), 1000 + seq); };

    feedp(hft::make_mold_add_packet(seq++, 42, 'S', 100, 1500000));   // rest 100 @ ask
    feedp(mold(seq++, body_x(42, 40)));                               // partial cancel -> 60 left
    CHECK(out.size() == 2, "X produced one message");
    CHECK(out[1].msg_type == T::ModifyOrder && out[1].qty == 60, "X is a reduction to 60, not a full cancel");
    CHECK(out[1].side == engine::Side::Sell && out[1].price == engine::Price(1500000) * 100, "X keeps side and price");

    feedp(mold(seq++, body_e(42, 25)));                               // execution -> 35 left
    CHECK(out.size() == 3 && out[2].msg_type == T::ModifyOrder && out[2].qty == 35, "E reduces to 35");

    feedp(mold(seq++, body_u(42, 43, 80, 1500100)));                  // replace -> cancel 42, new 43
    CHECK(out.size() == 5, "U produced two messages");
    CHECK(out[3].msg_type == T::CancelOrder && out[3].order_id == 42, "U cancels the original");
    CHECK(out[4].msg_type == T::NewOrder && out[4].order_id == 43 && out[4].qty == 80, "U adds the new ref");
    CHECK(out[4].side == engine::Side::Sell, "U inherits the original side");
    CHECK(out[4].price == engine::Price(1500100) * 100, "U uses the new price");

    feedp(mold(seq++, body_e(43, 80)));                               // full execution -> gone
    CHECK(out.size() == 6 && out[5].msg_type == T::CancelOrder && out[5].order_id == 43, "E to zero removes the order");

    feedp(hft::make_mold_add_packet(seq++, 50, 'B', 10, 1499900));
    feedp(mold(seq++, body_d(50)));
    CHECK(out.size() == 8 && out[7].msg_type == T::CancelOrder && out[7].order_id == 50, "D removes the order");

    feedp(mold(seq++, body_x(999, 5)));                               // unknown ref: no output, counted
    CHECK(out.size() == 8 && ad.counters().unknown_ref == 1, "unknown ref is counted, not forwarded");
    CHECK(ad.parse_errors() == 0, "no parse errors");
    for (const auto& m : out) CHECK(m.symbol == 7, "symbol mapped on every message");
}

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

    test_reductions_and_replace();

    TEST_EXIT();
}
