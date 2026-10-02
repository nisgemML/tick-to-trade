// tests/test_live_multicast.cpp
//
// Automated version of what tools/run_live_pipeline.cpp +
// tools/send_live_itch.cpp verify manually: a real UDP multicast socket,
// not a function call, delivering packets through GapBuffer -> ItchAdapter
// -> Pipeline -> matching engine -> fills. Single process (receiver on a
// background thread, sender on the test thread) so this runs in CI like
// any other ctest entry, without coordinating two separate process
// launches.
//
// Uses 239.1.1.2 (not .1) and port 15011 (not 15001) so this can run
// concurrently with a manual run_live_pipeline/send_live_itch session on
// the same machine without colliding.

#include "hft/pipeline.hpp"
#include "hft/itch_adapter.hpp"
#include "feed/receiver.hpp"
#include "feed/gap_buffer.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <net/if.h>
#include <netinet/in.h>
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

void send_packets(const char* group, uint16_t port, const char* iface, uint64_t n) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const in_addr iface_addr = interface_address(iface);
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr));
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    ::inet_pton(AF_INET, group, &dest.sin_addr);

    uint64_t ref = 1;
    for (uint64_t seq = 1; seq <= n; ++seq) {
        const char side = (seq % 2 == 0) ? 'B' : 'S';
        const int offset = static_cast<int>((seq * 17) % 21) - 10;
        const uint32_t px = 100'0000 + static_cast<uint32_t>(offset * 100);
        const auto pkt = hft::make_mold_add_packet(seq, ref++, side, 10 + (seq % 40), px);
        ::sendto(fd, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        ::usleep(100);
    }
    ::close(fd);
}

} // namespace

int main() {
    constexpr const char* kGroup = "239.1.1.2";
    constexpr uint16_t kPort = 15011;
    constexpr const char* kIface = "lo";
    constexpr uint64_t kN = 1000;

    hft::PipelineConfig pcfg;
    pcfg.log_path = "/tmp/test_live_multicast_fills.log";
    hft::Pipeline pipeline(pcfg);
    CHECK(pipeline.start());

    auto gap_buf = std::make_unique<feed::GapBuffer>(); // heap -- see BUGS_FOUND.md

    hft::ItchAdapterConfig icfg;
    hft::ItchAdapter adapter(icfg, [&](const engine::MarketDataMsg& m, uint64_t recv_ns) {
        pipeline.submit_with_ts(m, recv_ns);
    });
    adapter.attach(*gap_buf);

    feed::MulticastReceiver::Config rcfg;
    rcfg.multicast_group = kGroup;
    rcfg.port = kPort;
    rcfg.interface_name = kIface;
    feed::MulticastReceiver receiver(rcfg, *gap_buf);
    CHECK(receiver.open());

    std::atomic<bool> recv_running{true};
    std::thread recv_thread([&] { receiver.run(recv_running); });

    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // let the receiver actually be listening
    send_packets(kGroup, kPort, kIface, kN);
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // drain

    recv_running.store(false, std::memory_order_relaxed);
    recv_thread.join();
    receiver.close();
    pipeline.stop();

    const auto& r = pipeline.result();
    std::printf("recv_calls=%lu delivered=%lu parse_errors=%lu | submitted=%lu accepted=%lu fills=%lu\n",
                receiver.stat_recv_calls(), receiver.stat_messages_delivered(), receiver.stat_parse_errors(),
                r.events_submitted, r.events_accepted, r.fills);

    // This is the actual claim: a real socket, not a function call,
    // delivered every packet correctly through the whole pipeline.
    CHECK(receiver.stat_recv_calls() == kN);
    CHECK(receiver.stat_messages_delivered() == kN);
    CHECK(receiver.stat_parse_errors() == 0);
    CHECK(r.events_submitted == kN);
    CHECK(r.events_accepted == kN);
    CHECK(r.fills > 0); // exact count depends on matching dynamics; non-zero is the real assertion

    if (g_failures == 0) {
        std::printf("PASS: live multicast end-to-end, %lu/%lu packets delivered correctly\n", kN, kN);
        return 0;
    }
    std::printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
