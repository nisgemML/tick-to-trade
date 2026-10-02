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
//   defaults: 239.1.1.1 15001 lo 2000 200

#include "hft/itch_adapter.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <net/if.h>
#include <netinet/in.h>
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

        const ssize_t sent = ::sendto(fd, pkt.data(), pkt.size(), 0,
                                       reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (sent < 0) { std::perror("sendto"); break; }

        if (interval_us > 0) ::usleep(static_cast<useconds_t>(interval_us));
    }

    std::printf("send_live_itch: done\n");
    ::close(fd);
    return 0;
}
