#pragma once
// retransmit_client.hpp — MoldUDP64 re-request client.
//
// ── Protocol ──────────────────────────────────────────────────────────────────
//
// A MoldUDP64 receiver recovers lost messages by sending a 20-byte request
// datagram — session(10), first sequence(8, BE), message count(2, BE) — to
// the venue's re-request server. The server answers with ordinary
// MoldUDP64 downstream packets (same 20-byte header + length-prefixed
// messages), unicast to the address the request came from. Everything is
// UDP: requests and replies can be lost, and a reply may carry fewer
// messages than were asked for (one datagram's worth). The client does not
// track any of that — GapBuffer already re-requests whatever is still
// missing after retry_interval_ns, so loss of a request or reply is
// handled by the same mechanism as loss on the multicast line.
//
// ── Threading ─────────────────────────────────────────────────────────────────
//
// GapBuffer is single-threaded, so replies must be ingested on the same
// thread as the multicast line. Two ways to do that:
//   • MulticastReceiver::attach_retransmit_client(client) — the receiver's
//     busy-poll loop polls this socket too (what run_live_pipeline does).
//   • Call poll() yourself from the thread that owns the GapBuffer.
// send() is called from inside GapBuffer::ingest()/tick() via the
// OnRetransmitRequest callback, i.e. on that same thread. It is a single
// non-blocking sendto().
//
// ── Socket ────────────────────────────────────────────────────────────────────
//
// One UDP socket, connect()ed to the server: the kernel then delivers only
// datagrams whose source is the server, so stray unicast traffic cannot be
// mistaken for replies (session checking in GapBuffer is the second line
// of defence). Bound to `local_port` (0 = ephemeral).

#include "feed/gap_buffer.hpp"
#include "feed/wire_format.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace feed {

class RetransmitClient {
public:
    struct Config {
        std::string server_ip   = "127.0.0.1";
        uint16_t    server_port = 0;
        uint16_t    local_port  = 0;                  // 0 = ephemeral
        int         recv_buffer_bytes = 4 * 1024 * 1024;
        // Drain at most this many reply datagrams per poll() so a large
        // replay cannot starve the multicast line in a shared loop.
        int         max_datagrams_per_poll = 64;
    };

    explicit RetransmitClient(Config cfg) : cfg_(std::move(cfg)) {}
    ~RetransmitClient() { close(); }

    RetransmitClient(const RetransmitClient&)            = delete;
    RetransmitClient& operator=(const RetransmitClient&) = delete;

    [[nodiscard]] bool open() noexcept {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0) return report_error("socket");

        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF,
                     &cfg_.recv_buffer_bytes, sizeof(cfg_.recv_buffer_bytes));

        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_port        = htons(cfg_.local_port);
        local.sin_addr.s_addr = INADDR_ANY;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0)
            return report_error("bind");

        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port   = htons(cfg_.server_port);
        if (::inet_pton(AF_INET, cfg_.server_ip.c_str(), &server.sin_addr) != 1) {
            errno = EINVAL;
            return report_error("inet_pton(server_ip)");
        }
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0)
            return report_error("connect");
        return true;
    }

    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    // Route `gb`'s retransmit requests to this client.
    void attach(GapBuffer& gb) {
        gb.set_on_retransmit([this](const RetransmitRequest& r) { send(r); });
    }

    // Send one request datagram. Returns false if the send failed (counted;
    // GapBuffer will ask again after retry_interval_ns).
    bool send(const RetransmitRequest& req) noexcept {
        if (fd_ < 0 || req.count == 0) return false;
        uint8_t buf[RetransmitRequest::kSize];
        req.serialise(buf);
        const ssize_t n = ::send(fd_, buf, sizeof(buf), MSG_DONTWAIT);
        if (n != ssize_t(sizeof(buf))) { ++stat_send_errors_; return false; }
        ++stat_requests_sent_;
        stat_messages_requested_ += req.count;
        return true;
    }

    // Drain pending reply datagrams into `gb`. Returns messages delivered
    // downstream (replies also release anything buffered behind the gap).
    int poll(GapBuffer& gb) noexcept {
        if (fd_ < 0) return 0;
        int delivered = 0;
        for (int i = 0; i < cfg_.max_datagrams_per_poll; ++i) {
            const ssize_t n = ::recv(fd_, buf_, sizeof(buf_), MSG_DONTWAIT);
            if (n < 0) {
                // ECONNREFUSED: an earlier send hit a closed port (ICMP).
                // Not fatal — the server may come up; keep polling.
                if (errno == ECONNREFUSED) { ++stat_refused_; continue; }
                break;  // EAGAIN or a real error: nothing more now
            }
            ++stat_replies_;
            const int d = gb.ingest(buf_, std::size_t(n), monotonic_ns());
            if (d > 0) delivered += d;
            else if (d < 0) ++stat_reply_parse_errors_;
        }
        stat_messages_recovered_ += uint64_t(delivered);
        return delivered;
    }

    // ── Statistics ────────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t stat_requests_sent()      const noexcept { return stat_requests_sent_; }
    [[nodiscard]] uint64_t stat_messages_requested() const noexcept { return stat_messages_requested_; }
    [[nodiscard]] uint64_t stat_send_errors()        const noexcept { return stat_send_errors_; }
    [[nodiscard]] uint64_t stat_replies()            const noexcept { return stat_replies_; }
    [[nodiscard]] uint64_t stat_reply_parse_errors() const noexcept { return stat_reply_parse_errors_; }
    // Messages delivered downstream as a direct result of polling replies
    // (includes buffered messages a reply released).
    [[nodiscard]] uint64_t stat_messages_recovered() const noexcept { return stat_messages_recovered_; }
    [[nodiscard]] uint64_t stat_refused()            const noexcept { return stat_refused_; }

private:
    static uint64_t monotonic_ns() noexcept {
        timespec ts; ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
    }

    bool report_error(const char* call) noexcept {
        std::fprintf(stderr, "[RetransmitClient] %s: %s\n", call, std::strerror(errno));
        close();
        return false;
    }

    Config  cfg_;
    int     fd_ = -1;
    uint8_t buf_[65535];

    uint64_t stat_requests_sent_      = 0;
    uint64_t stat_messages_requested_ = 0;
    uint64_t stat_send_errors_        = 0;
    uint64_t stat_replies_            = 0;
    uint64_t stat_reply_parse_errors_ = 0;
    uint64_t stat_messages_recovered_ = 0;
    uint64_t stat_refused_            = 0;
};

} // namespace feed
