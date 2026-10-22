// tests/test_live_retransmit.cpp
//
// test_live_multicast with loss: a real UDP multicast socket where the
// sender deliberately never sends ~10% of packets (including the last few,
// so only heartbeats reveal them), and a real MoldUDP64 re-request server
// on loopback. The receiver's RetransmitClient, polled inside
// MulticastReceiver::run, must recover every message, so the pipeline sees
// the complete, in-order stream: GapBuffer -> ItchAdapter -> Pipeline.
//
// Group 239.1.1.3 / port 15021 so it can run alongside the other live test
// and a manual run_live_pipeline session.

#include "hft/pipeline.hpp"
#include "hft/itch_adapter.hpp"
#include "feed/receiver.hpp"
#include "feed/gap_buffer.hpp"
#include "feed/rerequest_server.hpp"
#include "feed/retransmit_client.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <net/if.h>
#include <netinet/in.h>
#include <random>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

in_addr interface_address(const char* ifname) {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    in_addr a{};
    if (s < 0) { a.s_addr = INADDR_ANY; return a; }
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (::ioctl(s, SIOCGIFADDR, &ifr) == 0) a = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr)->sin_addr;
    else a.s_addr = INADDR_ANY;
    ::close(s);
    return a;
}

} // namespace

int main() {
    constexpr const char* kGroup = "239.1.1.3";
    constexpr uint16_t kPort = 15021;
    constexpr const char* kIface = "lo";
    constexpr uint64_t kN = 1000;

    // Venue side: re-request server on its own thread.
    feed::RerequestServer::Config scfg;          // session: 10 spaces, matches make_mold_add_packet
    feed::RerequestServer server(scfg);
    CHECK(server.open());
    std::atomic<bool> server_running{true};
    std::thread server_thread([&] { server.serve(server_running); });

    hft::PipelineConfig pcfg;
    pcfg.log_path = "/tmp/test_live_retransmit_fills.log";
    hft::Pipeline pipeline(pcfg);
    CHECK(pipeline.start());

    // The receiver is up before the sender's first packet, so the stream
    // starts at 1: if seq 1 itself is lost it must be recovered too.
    feed::GapBuffer::Config gcfg;
    gcfg.expected_first_seq = 1;
    auto gap_buf = std::make_unique<feed::GapBuffer>(gcfg); // heap -- see BUGS_FOUND.md

    hft::ItchAdapterConfig icfg;
    hft::ItchAdapter adapter(icfg, [&](const engine::MarketDataMsg& m, uint64_t recv_ns) {
        pipeline.submit_with_ts(m, recv_ns);
    });
    adapter.attach(*gap_buf);
    uint64_t last_seq = 0; bool in_order = true;   // receive thread only
    // GapBuffer is owned by the receive thread; the sending thread below
    // only ever looks at these atomics (TSan caught an earlier version of
    // this test reading gap_buf->next_expected() across threads).
    std::atomic<uint64_t> delivered_through{0};
    std::atomic<bool>     eos_seen{false};
    // Wrap the adapter's callback to also check ordering at the feed level.
    gap_buf->set_on_message([&](const feed::MoldMessage& m) {
        if (m.seq_num != last_seq + 1) in_order = false;
        last_seq = m.seq_num;
        adapter.on_mold_message(m);
        delivered_through.store(m.seq_num, std::memory_order_release);
    });
    gap_buf->set_on_end_of_session([&](const feed::EndOfSessionEvent&) {
        eos_seen.store(true, std::memory_order_release);
    });

    feed::MulticastReceiver::Config rcfg;
    rcfg.multicast_group = kGroup;
    rcfg.port = kPort;
    rcfg.interface_name = kIface;
    feed::MulticastReceiver receiver(rcfg, *gap_buf);
    CHECK(receiver.open());

    feed::RetransmitClient::Config ccfg;
    ccfg.server_port = server.port();
    feed::RetransmitClient client(ccfg);
    CHECK(client.open());
    receiver.attach_retransmit_client(client);

    std::atomic<bool> recv_running{true};
    std::thread recv_thread([&] { receiver.run(recv_running); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── Sender: real multicast, deliberately lossy ───────────────────────────
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const in_addr iface_addr = interface_address(kIface);
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr));
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kPort);
    ::inet_pton(AF_INET, kGroup, &dest.sin_addr);
    auto send = [&](const uint8_t* p, std::size_t n) {
        ::sendto(fd, p, n, 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    };

    std::mt19937 rng(7);
    std::uniform_int_distribution<int> pct(0, 99);
    uint64_t dropped = 0;
    for (uint64_t seq = 1; seq <= kN; ++seq) {
        const char side = (seq % 2 == 0) ? 'B' : 'S';
        const int offset = static_cast<int>((seq * 17) % 21) - 10;
        const uint32_t px = 100'0000 + static_cast<uint32_t>(offset * 100);
        const auto pkt = hft::make_mold_add_packet(seq, seq, side, 10 + (seq % 40), px);
        server.record_packet(pkt.data(), pkt.size());
        if (seq > kN - 5 || pct(rng) < 10) { ++dropped; continue; }  // tail loss too
        send(pkt.data(), pkt.size());
        ::usleep(100);
    }
    // Quiet period: heartbeats carry the next sequence, then End of Session.
    auto ctl = [&](uint16_t count) {
        uint8_t b[feed::kMoldHeaderSize];
        feed::MoldHeader h{};
        std::memset(h.session, ' ', 10);
        h.seq_num = kN + 1; h.msg_count = count; h.serialise(b);
        send(b, sizeof(b));
    };
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!(delivered_through.load(std::memory_order_acquire) == kN &&
             eos_seen.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < until) {
        ctl(feed::kMoldHeartbeatCount);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (delivered_through.load(std::memory_order_acquire) == kN)
            ctl(feed::kMoldEndOfSessionCount);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ::close(fd);

    recv_running.store(false, std::memory_order_relaxed);
    recv_thread.join();
    server_running.store(false);
    server_thread.join();
    receiver.close();
    pipeline.stop();

    const auto& r = pipeline.result();
    std::printf("dropped=%lu gaps=%lu retx_sent=%lu replies=%lu | delivered=%lu "
                "submitted=%lu accepted=%lu fills=%lu session_complete=%d\n",
                dropped, gap_buf->stat_gaps(), client.stat_requests_sent(), client.stat_replies(),
                receiver.stat_messages_delivered(), r.events_submitted, r.events_accepted, r.fills,
                int(gap_buf->session_complete()));

    CHECK(dropped >= 5);
    CHECK(gap_buf->stat_gaps() > 0);                // loss actually happened on the wire
    CHECK(client.stat_requests_sent() > 0);
    CHECK(receiver.stat_messages_delivered() == kN); // ...and was fully recovered
    CHECK(last_seq == kN);
    CHECK(in_order);
    CHECK(gap_buf->session_complete());
    CHECK(r.events_submitted == kN);
    CHECK(r.events_accepted == kN);
    CHECK(r.fills > 0);

    if (g_failures == 0) {
        std::printf("PASS: lossy live multicast, %lu dropped on the wire, %lu/%lu recovered in order\n",
                    dropped, kN, kN);
        return 0;
    }
    std::printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
