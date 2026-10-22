// test_retransmit.cpp — MoldUDP64 gap recovery over real UDP sockets.
//
// GapBuffer + RetransmitClient against a RerequestServer on loopback:
//   1. Random multicast loss + tail loss (only heartbeats after the last
//      drop) + lost replies + replies capped well below the request size:
//      every message is recovered, exactly once, in order, with content.
//   2. No server listening: requests are sent and retried, nothing breaks,
//      ICMP port-unreachable (ECONNREFUSED) is absorbed.
//   3. Server for a different session: requests rejected, no data mixed in.
//   4. Wire layout of the request datagram matches the spec byte for byte.
//
// Single-threaded on purpose: the multicast "line" is a direct ingest()
// call, and server/client are polled in the same loop, so the test is
// deterministic apart from real socket timing.

#include "feed/gap_buffer.hpp"
#include "feed/rerequest_server.hpp"
#include "feed/retransmit_client.hpp"
#include "feed/wire_format.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>
#include <vector>

using namespace feed;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; } else { ++passed; } \
    } while(0)

static uint64_t now_ns() {
    timespec ts; ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
}

static const char kSess[11] = "RETXTEST  ";

// Message s: 8-byte body = s big-endian (content check on delivery).
static std::vector<uint8_t> make_packet(uint64_t seq, uint16_t count) {
    std::vector<uint8_t> b(kMoldHeaderSize + count * (2 + 8));
    MoldHeader h{}; std::memcpy(h.session, kSess, 10);
    h.seq_num = seq; h.msg_count = count; h.serialise(b.data());
    uint8_t* p = b.data() + kMoldHeaderSize;
    for (uint16_t i = 0; i < count; ++i) { put_be16(p, 8); put_be64(p + 2, seq + i); p += 10; }
    return b;
}
static std::vector<uint8_t> make_heartbeat(uint64_t next_seq) {
    std::vector<uint8_t> b(kMoldHeaderSize);
    MoldHeader h{}; std::memcpy(h.session, kSess, 10);
    h.seq_num = next_seq; h.msg_count = 0; h.serialise(b.data());
    return b;
}

static GapBuffer::Config fast_cfg() {
    GapBuffer::Config c;
    c.gap_timeout_ns       = 50'000;     // 50 µs
    c.retry_interval_ns    = 500'000;    // 500 µs
    c.max_retransmit_count = 100;
    return c;
}

// ── 1. Full recovery under multicast loss, tail loss and reply loss ──────────

static void test_full_recovery() {
    RerequestServer::Config scfg;
    std::memcpy(scfg.session, kSess, 10);
    scfg.max_payload_bytes    = 200;   // ~18 msgs/reply: forces partial replies
    scfg.drop_every_nth_reply = 3;     // lose a third of replies
    RerequestServer server(scfg);
    CHECK(server.open(), "server open");

    RetransmitClient::Config ccfg;
    ccfg.server_port = server.port();
    RetransmitClient client(ccfg);
    CHECK(client.open(), "client open");

    auto gb = std::make_unique<GapBuffer>(fast_cfg());
    client.attach(*gb);
    std::vector<uint64_t> got; bool content_ok = true;
    gb->set_on_message([&](const MoldMessage& m) {
        got.push_back(m.seq_num);
        if (m.length != 8 || be64(m.body) != m.seq_num) content_ok = false;
    });

    constexpr uint64_t kN = 3000;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> pct(0, 99);
    uint64_t seq = 1, dropped_msgs = 0;
    while (seq <= kN) {
        const uint16_t n = uint16_t(std::min<uint64_t>(1 + (seq % 4), kN + 1 - seq));
        const auto pkt = make_packet(seq, n);
        server.record_packet(pkt.data(), pkt.size());   // venue always has it
        // Drop 10% of packets on the "line", and the final 25 messages
        // entirely (tail loss: nothing after them but heartbeats).
        const bool tail = seq + n > kN - 25;
        if (tail || pct(rng) < 10) dropped_msgs += n;
        else gb->ingest(pkt.data(), pkt.size(), now_ns());
        seq += n;
        server.poll(); client.poll(*gb); gb->tick(now_ns());
    }
    CHECK(dropped_msgs > 300, "test actually dropped a meaningful amount");

    // Feed goes quiet: heartbeats only, every ~1 ms, as a venue would.
    const uint64_t deadline = now_ns() + 3'000'000'000ULL;
    uint64_t next_hb = 0;
    while (gb->next_expected() <= kN && now_ns() < deadline) {
        const uint64_t t = now_ns();
        if (t >= next_hb) {
            const auto hb = make_heartbeat(kN + 1);
            gb->ingest(hb.data(), hb.size(), t);
            next_hb = t + 1'000'000;
        }
        gb->tick(t);
        server.poll();
        client.poll(*gb);
    }

    bool in_order = got.size() == kN;
    for (uint64_t i = 0; in_order && i < kN; ++i) in_order = (got[i] == i + 1);
    CHECK(in_order,            "all messages delivered exactly once, in order");
    CHECK(content_ok,          "delivered bodies are the right messages");
    CHECK(!gb->in_gap(),       "no gap remaining");
    CHECK(server.stat_replies_dropped() > 0, "reply loss was exercised");
    CHECK(client.stat_requests_sent() > server.stat_replies_sent(),
          "lost/partial replies were re-requested");
    CHECK(client.stat_messages_recovered() >= dropped_msgs,
          "everything dropped on the line came back via retransmit");
    std::printf("  full recovery: dropped=%lu requests=%lu replies=%lu (dropped %lu) "
                "recovered=%lu buffer_overflows=%lu\n",
                dropped_msgs, client.stat_requests_sent(), server.stat_replies_sent(),
                server.stat_replies_dropped(), client.stat_messages_recovered(),
                gb->stat_buffer_overflows());
}

// ── 2. No server: requests retried, no crash ─────────────────────────────────

static void test_no_server() {
    // Grab an ephemeral port, then close it so nothing listens there.
    uint16_t dead_port;
    {
        RerequestServer::Config s; RerequestServer tmp(s);
        CHECK(tmp.open(), "tmp open");
        dead_port = tmp.port();
    }
    RetransmitClient::Config ccfg; ccfg.server_port = dead_port;
    RetransmitClient client(ccfg);
    CHECK(client.open(), "client open");

    auto gb = std::make_unique<GapBuffer>(fast_cfg());
    client.attach(*gb);
    auto p1 = make_packet(1, 1), p5 = make_packet(5, 1);
    gb->ingest(p1.data(), p1.size(), now_ns());
    gb->ingest(p5.data(), p5.size(), now_ns());

    const uint64_t end = now_ns() + 20'000'000;   // 20 ms
    while (now_ns() < end) { gb->tick(now_ns()); client.poll(*gb); }
    CHECK(client.stat_requests_sent() >= 5, "requests keep being retried");
    CHECK(gb->in_gap() && gb->next_expected() == 2, "gap still open, nothing invented");
}

// ── 3. Server for a different session ignores us ─────────────────────────────

static void test_wrong_session_server() {
    RerequestServer::Config scfg;
    std::memcpy(scfg.session, "OTHERSESS ", 10);
    RerequestServer server(scfg);
    CHECK(server.open(), "server open");
    RetransmitClient::Config ccfg; ccfg.server_port = server.port();
    RetransmitClient client(ccfg);
    CHECK(client.open(), "client open");

    auto gb = std::make_unique<GapBuffer>(fast_cfg());
    client.attach(*gb);
    auto p1 = make_packet(1, 1), p2 = make_packet(2, 1), p3 = make_packet(3, 1);
    server.record_packet(p1.data(), p1.size());
    server.record_packet(p2.data(), p2.size());
    gb->ingest(p1.data(), p1.size(), now_ns());
    gb->ingest(p3.data(), p3.size(), now_ns());

    const uint64_t end = now_ns() + 10'000'000;
    while (now_ns() < end) { gb->tick(now_ns()); server.poll(); client.poll(*gb); }
    CHECK(server.stat_requests() > 0 && server.stat_rejected() == server.stat_requests(),
          "every request for the wrong session rejected");
    CHECK(gb->next_expected() == 2, "nothing delivered from a mismatched server");
}

// ── 4. Request datagram layout ───────────────────────────────────────────────

static void test_request_layout() {
    RetransmitRequest r;
    std::memcpy(r.session, "ABCDEFGHIJ", 10);
    r.first_seq = 0x0102030405060708ULL;
    r.count     = 0x0A0B;
    uint8_t b[RetransmitRequest::kSize];
    r.serialise(b);
    const uint8_t want[20] = { 'A','B','C','D','E','F','G','H','I','J',
                               1,2,3,4,5,6,7,8, 0x0A,0x0B };
    CHECK(std::memcmp(b, want, 20) == 0, "request bytes match MoldUDP64 spec layout");
    RetransmitRequest back;
    CHECK(back.parse(b, 20) && back.first_seq == r.first_seq && back.count == r.count,
          "request round-trips");
}

int main() {
    std::printf("=== Retransmit (UDP re-request) Tests ===\n\n");
    test_request_layout();
    test_full_recovery();
    test_no_server();
    test_wrong_session_server();
    std::printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
