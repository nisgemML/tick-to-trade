#pragma once
// rerequest_server.hpp — Minimal MoldUDP64 re-request server (simulation).
//
// The venue side of retransmit_client.hpp, for tests and local demos: a
// sender records every message it publishes, and this server answers
// MoldUDP64 request datagrams from that store with ordinary downstream
// packets, unicast to the requester.
//
// Behaviour follows the protocol, including the parts a client must cope
// with:
//   • A reply carries at most one datagram's worth of messages
//     (max_payload_bytes), so a large request is answered partially and
//     the client must re-request the remainder.
//   • Requests for another session, or for sequences not yet published,
//     are ignored (counted).
//   • `drop_every_nth_reply` simulates reply loss on the unicast path.
//
// Not a production component: one socket, a mutex-protected in-memory
// store, poll()ed by whoever owns it. Thread-safe for one publisher thread
// (record()) plus one serving thread (poll()/serve()).

#include "feed/wire_format.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace feed {

class RerequestServer {
public:
    struct Config {
        std::string bind_ip  = "127.0.0.1";
        uint16_t    port     = 0;          // 0 = ephemeral; see port()
        char        session[10];
        std::size_t max_payload_bytes   = 1400;  // per reply datagram
        uint32_t    drop_every_nth_reply = 0;    // 0 = never drop
        Config() noexcept { std::memset(session, ' ', 10); }
    };

    explicit RerequestServer(Config cfg) : cfg_(std::move(cfg)) {}
    ~RerequestServer() { close(); }
    RerequestServer(const RerequestServer&)            = delete;
    RerequestServer& operator=(const RerequestServer&) = delete;

    [[nodiscard]] bool open() noexcept {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0) return report_error("socket");
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port   = htons(cfg_.port);
        if (::inet_pton(AF_INET, cfg_.bind_ip.c_str(), &a.sin_addr) != 1) {
            errno = EINVAL; return report_error("inet_pton");
        }
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0)
            return report_error("bind");
        socklen_t len = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        return true;
    }

    void close() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    [[nodiscard]] uint16_t port() const noexcept { return port_; }

    // ── Publishing side ───────────────────────────────────────────────────────

    // Record message `seq` (sequences must be recorded 1, 2, 3, ...).
    void record(uint64_t seq, const uint8_t* body, uint16_t len) {
        std::lock_guard<std::mutex> lk(mu_);
        if (seq != store_.size() + 1) return;   // out of order: ignore
        store_.emplace_back(body, body + len);
    }

    // Record every message in a downstream packet the sender published.
    void record_packet(const uint8_t* pkt, std::size_t len) {
        MoldHeader hdr;
        if (!hdr.parse(pkt, len) || hdr.data_count() == 0) return;
        const uint8_t* p   = pkt + kMoldHeaderSize;
        const uint8_t* end = pkt + len;
        for (uint16_t i = 0; i < hdr.msg_count && p + 2 <= end; ++i) {
            const uint16_t ml = be16(p); p += 2;
            if (p + ml > end) break;
            record(hdr.seq_num + i, p, ml);
            p += ml;
        }
    }

    // ── Serving side ──────────────────────────────────────────────────────────

    // Handle all pending requests without blocking. Returns requests served.
    int poll() noexcept {
        if (fd_ < 0) return 0;
        int served = 0;
        for (;;) {
            uint8_t     req_buf[64];
            sockaddr_in from{};
            socklen_t   flen = sizeof(from);
            const ssize_t n = ::recvfrom(fd_, req_buf, sizeof(req_buf), MSG_DONTWAIT,
                                         reinterpret_cast<sockaddr*>(&from), &flen);
            if (n < 0) break;
            ++stat_requests_;
            RetransmitRequest req;
            if (!req.parse(req_buf, std::size_t(n)) ||
                std::memcmp(req.session, cfg_.session, 10) != 0 ||
                req.first_seq == 0 || req.count == 0) {
                ++stat_rejected_;
                continue;
            }
            if (answer(req, from)) ++served;
        }
        return served;
    }

    // Serve until `running` goes false (sleeps in ::poll, ~1 ms granularity).
    void serve(const std::atomic<bool>& running) noexcept {
        while (running.load(std::memory_order_relaxed)) {
            pollfd pfd{ fd_, POLLIN, 0 };
            if (::poll(&pfd, 1, 1) > 0) poll();
        }
    }

    [[nodiscard]] uint64_t stat_requests()         const noexcept { return stat_requests_; }
    [[nodiscard]] uint64_t stat_rejected()         const noexcept { return stat_rejected_; }
    [[nodiscard]] uint64_t stat_replies_sent()     const noexcept { return stat_replies_sent_; }
    [[nodiscard]] uint64_t stat_replies_dropped()  const noexcept { return stat_replies_dropped_; }
    [[nodiscard]] uint64_t stat_messages_replayed() const noexcept { return stat_messages_replayed_; }

private:
    bool answer(const RetransmitRequest& req, const sockaddr_in& to) noexcept {
        uint8_t  pkt[65535];
        std::size_t off = kMoldHeaderSize;
        uint16_t count = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const uint64_t avail_end = store_.size() + 1;       // one past last
            if (req.first_seq >= avail_end) { ++stat_rejected_; return false; }
            uint64_t s = req.first_seq;
            const uint64_t want_end = std::min<uint64_t>(avail_end, req.first_seq + req.count);
            for (; s < want_end; ++s) {
                const auto& m = store_[s - 1];
                if (off + 2 + m.size() > cfg_.max_payload_bytes && count > 0) break;
                put_be16(pkt + off, uint16_t(m.size()));
                std::memcpy(pkt + off + 2, m.data(), m.size());
                off += 2 + m.size();
                ++count;
            }
        }
        MoldHeader hdr{};
        std::memcpy(hdr.session, cfg_.session, 10);
        hdr.seq_num   = req.first_seq;
        hdr.msg_count = count;
        hdr.serialise(pkt);

        ++reply_counter_;
        if (cfg_.drop_every_nth_reply && reply_counter_ % cfg_.drop_every_nth_reply == 0) {
            ++stat_replies_dropped_;
            return true;
        }
        if (::sendto(fd_, pkt, off, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) ==
            ssize_t(off)) {
            ++stat_replies_sent_;
            stat_messages_replayed_ += count;
        }
        return true;
    }

    bool report_error(const char* call) noexcept {
        std::fprintf(stderr, "[RerequestServer] %s: %s\n", call, std::strerror(errno));
        close();
        return false;
    }

    Config   cfg_;
    int      fd_   = -1;
    uint16_t port_ = 0;

    std::mutex mu_;
    std::vector<std::vector<uint8_t>> store_;   // store_[seq - 1]

    uint64_t reply_counter_          = 0;
    uint64_t stat_requests_          = 0;
    uint64_t stat_rejected_          = 0;
    uint64_t stat_replies_sent_      = 0;
    uint64_t stat_replies_dropped_   = 0;
    uint64_t stat_messages_replayed_ = 0;
};

} // namespace feed
