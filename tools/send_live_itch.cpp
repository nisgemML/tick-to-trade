// tools/send_live_itch.cpp
//
// Companion to run_live_pipeline: encodes real MoldUDP64/ITCH AddOrder
// packets (using itch_adapter.hpp's make_mold_add_packet(), the same
// encoder the integration tests already trust) and sends them over an
// actual UDP multicast socket -- the other half of proving the live
// receive path end-to-end, not just that the pipeline accepts
// function-call input.
//
// Usage: send_live_itch [group] [port] [iface] [n_messages] [interval_us]
//                       [drop_pct] [rerequest_port]
//   defaults: 239.1.1.1 15001 lo 2000 200 0 0
//
// With rerequest_port > 0 this also acts as the venue's MoldUDP64
// re-request server on 127.0.0.1:<rerequest_port> (feed/rerequest_server.hpp):
// every packet is recorded before it is (maybe) sent, drop_pct% of
// multicast packets are deliberately never sent, and after the last data
// packet the sender keeps emitting heartbeats (seq = next sequence, as the
// spec requires — this is what lets the receiver detect tail loss) and
// keeps serving re-requests for a linger period, then sends End of
// Session. Pair with: run_live_pipeline ... 127.0.0.1:<rerequest_port>

#include "hft/itch_adapter.hpp"
#include "feed/rerequest_server.hpp"
#include "feed/wire_format.hpp"

#include <atomic>
#include <chrono>
#include <random>

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <net/if.h>
#include <netinet/in.h>
#include <memory>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
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
}

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "239.1.1.1";
    const uint16_t    port  = argc > 2 ? static_cast<uint16_t>(std::stoi(argv[2])) : 15001;
    const std::string iface = argc > 3 ? argv[3] : "lo";
    const uint64_t  n_msgs  = argc > 4 ? std::stoull(argv[4]) : 2000;
    const int   interval_us = argc > 5 ? std::stoi(argv[5]) : 200;
    const int      drop_pct = argc > 6 ? std::stoi(argv[6]) : 0;
    const uint16_t rr_port  = argc > 7 ? static_cast<uint16_t>(std::stoi(argv[7])) : 0;

    // Optional venue-side re-request server, served on its own thread.
    std::unique_ptr<feed::RerequestServer> rr;
    std::atomic<bool> rr_running{true};
    std::thread rr_thread;
    if (rr_port > 0) {
        feed::RerequestServer::Config rc;   // session = 10 spaces, as make_mold_add_packet uses
        rc.port = rr_port;
        rr = std::make_unique<feed::RerequestServer>(rc);
        if (!rr->open()) return 1;
        rr_thread = std::thread([&] { rr->serve(rr_running); });
        std::printf("send_live_itch: re-request server on 127.0.0.1:%u, dropping %d%% of packets\n",
                    rr_port, drop_pct);
    } else if (drop_pct > 0) {
        std::fprintf(stderr, "send_live_itch: drop_pct without rerequest_port means unrecoverable loss\n");
    }
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> pct(0, 99);
    uint64_t dropped = 0;

    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { std::perror("socket"); return 1; }

    const in_addr iface_addr = interface_address(iface.c_str());
    if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr)) < 0) {
        std::perror("IP_MULTICAST_IF");
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    ::inet_pton(AF_INET, group.c_str(), &dest.sin_addr);

    std::printf("send_live_itch: sending %lu AddOrder packets to %s:%u (%s)\n",
                n_msgs, group.c_str(), port, iface.c_str());

    uint64_t ref = 1;
    uint32_t mid_px = 100'0000; // matches hft's price scaling convention
    for (uint64_t seq = 1; seq <= n_msgs; ++seq) {
        const char side = (seq % 2 == 0) ? 'B' : 'S';
        const int offset = static_cast<int>((seq * 17) % 21) - 10;
        const uint32_t px = mid_px + static_cast<uint32_t>(offset * 100);
        const auto pkt = hft::make_mold_add_packet(seq, ref++, side, 10 + (seq % 40), px);

        if (rr) rr->record_packet(pkt.data(), pkt.size());
        if (drop_pct > 0 && pct(rng) < drop_pct) { ++dropped; continue; }

        const ssize_t sent = ::sendto(fd, pkt.data(), pkt.size(), 0,
                                       reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (sent < 0) { std::perror("sendto"); break; }

        if (interval_us > 0) ::usleep(static_cast<useconds_t>(interval_us));
    }

    // Heartbeats then End of Session. seq = next sequence to be sent.
    auto send_ctl = [&](uint16_t count) {
        uint8_t b[feed::kMoldHeaderSize];
        feed::MoldHeader h{};
        std::memset(h.session, ' ', 10);
        h.seq_num = n_msgs + 1;
        h.msg_count = count;
        h.serialise(b);
        ::sendto(fd, b, sizeof(b), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    };
    if (rr) {
        const auto linger_end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < linger_end) {
            send_ctl(feed::kMoldHeartbeatCount);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    for (int i = 0; i < 3; ++i) send_ctl(feed::kMoldEndOfSessionCount);

    if (rr) {
        rr_running.store(false);
        rr_thread.join();
        std::printf("send_live_itch: done (dropped %lu, re-requests %lu, replies %lu, replayed %lu msgs)\n",
                    dropped, rr->stat_requests(), rr->stat_replies_sent(), rr->stat_messages_replayed());
    } else {
        std::printf("send_live_itch: done\n");
    }
    ::close(fd);
    return 0;
}
