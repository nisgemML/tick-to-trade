#pragma once
// gap_buffer.hpp — MoldUDP64 sequencing: gap detection, out-of-order
// buffering, retransmit-request generation and session lifecycle.
//
// ── The problem ───────────────────────────────────────────────────────────────
//
// UDP multicast is unreliable.  Packets can be:
//   • Dropped by a switch buffer overflow during traffic bursts
//   • Reordered by asymmetric routing (rare but happens in co-lo)
//   • Duplicated by network equipment, or by the A/B line pair
//
// The receiver must detect all three cases and handle them without
// blocking the hot path.  Delivery to downstream must be in-order and
// without duplicates.
//
// ── MoldUDP64 rules this class implements ────────────────────────────────────
//
//   • The header sequence number is the sequence of the FIRST message in
//     the packet; message i in the packet has sequence seq + i.
//   • Heartbeat (count == 0) and End of Session (count == 0xFFFF) carry
//     no messages; their sequence number is the NEXT sequence the server
//     will send. A heartbeat with seq > next_expected therefore reveals a
//     gap on its own — this is the only way tail loss (the last packets
//     before the feed goes quiet) is ever detected.
//   • End of Session: no further messages will be sent for this session.
//     Messages before it may still be missing and are still recoverable.
//   • Every packet carries the 10-byte session. Packets for a different
//     session belong to a different sequence space and must never be
//     mixed into this one.
//
// ── Design ────────────────────────────────────────────────────────────────────
//
// One code path for every data packet: walk its messages, and for each
// message sequence s:
//   s <  next_expected  → duplicate, skip
//   s == next_expected  → deliver, then flush any buffered successors
//   s >  next_expected  → buffer (per MESSAGE, in slot s % kCapacity)
//
// Buffering per message (not per packet) is what makes overlapping
// retransmits correct: a retransmit is free to re-packetise the replayed
// range differently from the original multicast, so a replay of [2..6]
// can overlap a buffered original packet [5..8] mid-packet. With
// per-message slots that overlap is just "5 and 6 are duplicates".
//
// Retransmit requests: when a gap is first detected, record the time. If
// the gap is not filled within `gap_timeout_ns`, emit a RetransmitRequest
// covering the contiguous missing run starting at next_expected, bounded
// by the highest sequence the feed has evidenced (so we never ask for
// sequences that don't exist yet), and by `max_retransmit_count`.
// Retries every `retry_interval_ns` until filled. When a fill closes one
// hole but another remains behind it, the gap stays open at the new hole
// with the ORIGINAL detection time — that loss is not new.
//
// ── Capacity choice ───────────────────────────────────────────────────────────
//
// Slots are indexed by MoldUDP64 sequence number (which counts MESSAGES,
// not packets), so the buffer covers a window of 4096 sequence numbers
// past next_expected. How much wall-clock time that buys depends entirely
// on the feed rate R: window = 4096 / R. At 1M msg/s that is ~4 ms; at a
// 10M msg/s burst it is ~0.4 ms — SHORTER than a typical sub-millisecond-
// to-millisecond retransmit round trip. A message that arrives beyond the
// window is dropped and counted (stat_buffer_overflows), never silent.
// Because the feed's sequence high-water mark still advances, the dropped
// range is simply requested again by later retransmit rounds once the
// window has moved — overflow costs extra retransmit traffic, it does not
// lose data, provided a retransmission server is reachable. The real
// defence at peak rates is A/B line arbitration (feed_arbitrator.hpp).
//
// Memory: 4096 * sizeof(BufferedMessage) ≈ 6.2 MB, allocated inline once,
// never on the hot path. Heap-allocate the GapBuffer (it is too large for
// a default thread stack).
//
// ── Threading ─────────────────────────────────────────────────────────────────
//
// Not thread-safe. ingest() and tick() must be called from one thread;
// multicast lines AND retransmit responses are all fed in from that same
// thread (MulticastReceiver::run does this when a RetransmitClient is
// attached).

#include "feed/wire_format.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>

namespace feed {

// ── Events reported to the application ───────────────────────────────────────

struct GapEvent {
    uint64_t first_missing_seq;
    uint64_t last_missing_seq;   // inclusive
    uint64_t detected_at_ns;     // CLOCK_MONOTONIC nanoseconds
};

struct SessionChangeEvent {
    char     old_session[10];
    char     new_session[10];
    uint64_t new_first_seq;      // next_expected in the new session
};

struct EndOfSessionEvent {
    char     session[10];
    uint64_t final_next_seq;     // one past the last message of the session
};

// ── Buffered message slot ─────────────────────────────────────────────────────

// Largest single MoldUDP64 message we will hold in the out-of-order
// buffer. A message must fit in one datagram, so on a standard 1500-byte
// MTU this is generous; larger messages are still delivered when they
// arrive in order — only buffering is bounded.
static constexpr std::size_t kMaxMessageBytes = 1500;
// Kept for source compatibility with code that referenced the old name.
static constexpr std::size_t kMaxPacketBytes  = kMaxMessageBytes;

struct BufferedMessage {
    uint64_t seq_num  = 0;
    uint64_t recv_ns  = 0;           // original packet arrival time, preserved
                                      // across buffering so downstream latency
                                      // measurement reflects true wire arrival,
                                      // not flush time.
    uint16_t length   = 0;
    bool     occupied = false;
    uint8_t  data[kMaxMessageBytes];
};

// ── GapBuffer ─────────────────────────────────────────────────────────────────

class GapBuffer {
public:
    // Capacity must be a power of two for fast modulo via bitmask.
    static constexpr std::size_t kCapacity = 4096;
    static_assert((kCapacity & (kCapacity - 1)) == 0);
    static constexpr std::size_t kMask = kCapacity - 1;

    // OnMessage: called for each in-order MoldMessage delivered downstream.
    // OnGap: called once when a new loss episode is first detected.
    // OnRetransmitRequest: called when a RetransmitRequest should be sent.
    // OnSessionChange: called when the feed moved to a new session and
    //   accept_session_change is set; downstream state (order books!) built
    //   from the old session is no longer valid.
    // OnEndOfSession: called once when an End of Session packet is seen.
    using OnMessage           = std::function<void(const MoldMessage&)>;
    using OnGap               = std::function<void(const GapEvent&)>;
    using OnRetransmitRequest = std::function<void(const RetransmitRequest&)>;
    using OnSessionChange     = std::function<void(const SessionChangeEvent&)>;
    using OnEndOfSession      = std::function<void(const EndOfSessionEvent&)>;

    struct Config {
        uint64_t gap_timeout_ns       = 100'000;   // 100 µs before first retransmit
        uint64_t retry_interval_ns    = 1'000'000; // 1 ms between retransmit retries
        uint16_t max_retransmit_count = 100;       // max messages per retransmit request
        // Expected session. All spaces = learn it from the first packet.
        // Anything else pins the buffer to that session from the start.
        char     session[10];
        // What to do when a packet arrives for a session other than the
        // current one:
        //   false (default) — drop it and count it (stat_session_mismatches).
        //                     Safe against stray traffic on the group.
        //   true            — treat it as a session rollover: discard all
        //                     buffered state, resync to the new session's
        //                     sequence, fire OnSessionChange.
        bool     accept_session_change = false;
        // Where the stream starts:
        //   0 (default) — sync to the first packet seen (late join: no
        //                 attempt to recover anything before it).
        //   N           — expect sequence N first. Anything lost before
        //                 the first packet that arrives is treated as a
        //                 gap and requested. Use 1 when the receiver is up
        //                 before session open, or to replay a whole
        //                 session after a mid-session start (see
        //                 docs/loss-handling.md §4).
        uint64_t expected_first_seq = 0;

        Config() noexcept { std::memset(session, ' ', 10); }
    };

    explicit GapBuffer(Config cfg = {})
        : cfg_(cfg)
    {
        std::memcpy(session_, cfg_.session, 10);
        session_synced_ = !is_blank(cfg_.session);
        slots_.fill({});
        reset_sequence_state();
    }

    void set_on_message(OnMessage cb)                { on_message_     = std::move(cb); }
    void set_on_gap(OnGap cb)                        { on_gap_         = std::move(cb); }
    void set_on_retransmit(OnRetransmitRequest cb)   { on_retx_        = std::move(cb); }
    void set_on_session_change(OnSessionChange cb)   { on_session_chg_ = std::move(cb); }
    void set_on_end_of_session(OnEndOfSession cb)    { on_eos_         = std::move(cb); }

    // ── Primary entry point ───────────────────────────────────────────────────
    //
    // Call this for every received MoldUDP64 datagram — from any multicast
    // line, and for retransmit responses.
    // `now_ns` is the packet receive timestamp (CLOCK_MONOTONIC nanoseconds).
    // `payload` is the datagram bytes starting at the MoldUDP64 header.
    //
    // Returns: number of messages delivered downstream, -1 on parse error.

    int ingest(const uint8_t* payload, std::size_t len, uint64_t now_ns) noexcept {
        MoldHeader hdr;
        if (!hdr.parse(payload, len)) return -1;

        if (!check_session(hdr)) return 0;

        const bool eos       = hdr.is_end_of_session();
        const bool heartbeat = hdr.is_heartbeat();

        // First packet of the session (or of a learned session): sync.
        // Sequence numbers start at 1; a 0 carries no position information.
        if (!seq_synced_ && hdr.seq_num != 0) {
            next_expected_ = hdr.seq_num;
            high_water_    = hdr.seq_num;
            seq_synced_    = true;
        }

        if (heartbeat || eos) {
            if (heartbeat) ++stat_heartbeats_;
            if (seq_synced_) {
                // seq is the NEXT sequence the server will send.
                note_high_water(hdr.seq_num);
                if (hdr.seq_num > next_expected_) handle_gap(hdr.seq_num, now_ns);
            }
            if (eos) on_end_of_session(hdr);
            check_gap_timeout(now_ns);
            return 0;
        }

        if (!seq_synced_) { ++stat_malformed_; return -1; }  // data with seq 0

        ++stat_packets_;
        stat_messages_ += hdr.msg_count;

        const uint64_t pkt_first = hdr.seq_num;
        const uint64_t pkt_end   = pkt_first + hdr.msg_count;   // one past last
        note_high_water(pkt_end);

        // Whole packet already delivered.
        if (pkt_end <= next_expected_) {
            ++stat_duplicates_;
            check_gap_timeout(now_ns);
            return 0;
        }

        // Packet starts past next_expected: a gap precedes it.
        if (pkt_first > next_expected_) handle_gap(pkt_first, now_ns);

        const int delivered = walk_packet(hdr, payload, len, now_ns);
        update_gap_state(now_ns);
        check_gap_timeout(now_ns);
        return delivered;
    }

    // Periodic tick — call at ~100µs intervals to detect stalled gaps.
    void tick(uint64_t now_ns) noexcept { check_gap_timeout(now_ns); }

    // Discard all sequencing state and buffered messages, keeping config
    // and callbacks. Next packet re-syncs (to cfg.session if pinned).
    void reset() noexcept {
        for (auto& s : slots_) s.occupied = false;
        std::memcpy(session_, cfg_.session, 10);
        session_synced_ = !is_blank(cfg_.session);
        reset_sequence_state();
    }

    // ── State / statistics ────────────────────────────────────────────────────

    [[nodiscard]] uint64_t next_expected()   const noexcept { return next_expected_; }
    // One past the highest sequence number the feed has evidenced so far
    // (via data, heartbeat or end-of-session).
    [[nodiscard]] uint64_t high_water()      const noexcept { return high_water_; }
    [[nodiscard]] bool     in_gap()          const noexcept { return in_gap_; }
    [[nodiscard]] bool     end_of_session()  const noexcept { return eos_seen_; }
    // True once End of Session has been seen AND every message before it
    // has been delivered.
    [[nodiscard]] bool     session_complete() const noexcept {
        return eos_seen_ && !in_gap_ && next_expected_ >= eos_seq_;
    }
    [[nodiscard]] std::string_view session() const noexcept { return { session_, 10 }; }

    [[nodiscard]] uint64_t stat_packets()    const noexcept { return stat_packets_; }
    [[nodiscard]] uint64_t stat_messages()   const noexcept { return stat_messages_; }
    [[nodiscard]] uint64_t stat_gaps()       const noexcept { return stat_gaps_; }
    [[nodiscard]] uint64_t stat_duplicates() const noexcept { return stat_duplicates_; }
    [[nodiscard]] uint64_t stat_delivered()  const noexcept { return stat_delivered_; }
    [[nodiscard]] uint64_t stat_retransmit_requests() const noexcept { return stat_retx_; }
    [[nodiscard]] uint64_t stat_heartbeats() const noexcept { return stat_heartbeats_; }
    [[nodiscard]] uint64_t stat_buffer_overflows()   const noexcept { return stat_buffer_overflows_; }
    [[nodiscard]] uint64_t stat_session_mismatches() const noexcept { return stat_session_mismatches_; }
    [[nodiscard]] uint64_t stat_session_changes()    const noexcept { return stat_session_changes_; }
    [[nodiscard]] uint64_t stat_malformed()  const noexcept { return stat_malformed_; }

private:
    static bool is_blank(const char* s) noexcept {
        for (int i = 0; i < 10; ++i) if (s[i] != ' ') return false;
        return true;
    }

    void reset_sequence_state() noexcept {
        seq_synced_         = cfg_.expected_first_seq != 0;
        next_expected_      = cfg_.expected_first_seq;
        high_water_         = cfg_.expected_first_seq;
        in_gap_             = false;
        gap_detected_at_ns_ = 0;
        gap_last_retx_ns_   = 0;
        retx_requested_end_ = 0;
        eos_seen_           = false;
        eos_seq_            = 0;
    }

    // ── Session handling ──────────────────────────────────────────────────────

    // Returns false if the packet must be ignored.
    bool check_session(const MoldHeader& hdr) noexcept {
        if (!session_synced_) {
            std::memcpy(session_, hdr.session, 10);
            session_synced_ = true;
            return true;
        }
        if (std::memcmp(session_, hdr.session, 10) == 0) return true;

        if (!cfg_.accept_session_change) {
            ++stat_session_mismatches_;
            return false;
        }

        // Session rollover: the old sequence space is gone. Everything
        // buffered belongs to it and must not leak into the new one.
        SessionChangeEvent ev{};
        std::memcpy(ev.old_session, session_, 10);
        std::memcpy(ev.new_session, hdr.session, 10);
        ev.new_first_seq = hdr.seq_num;

        for (auto& s : slots_) s.occupied = false;
        std::memcpy(session_, hdr.session, 10);
        reset_sequence_state();
        ++stat_session_changes_;
        if (on_session_chg_) on_session_chg_(ev);
        return true;
    }

    void on_end_of_session(const MoldHeader& hdr) noexcept {
        if (eos_seen_) return;
        eos_seen_ = true;
        eos_seq_  = hdr.seq_num;
        if (on_eos_) {
            EndOfSessionEvent ev{};
            std::memcpy(ev.session, session_, 10);
            ev.final_next_seq = hdr.seq_num;
            on_eos_(ev);
        }
    }

    void note_high_water(uint64_t next_seq) noexcept {
        if (next_seq > high_water_) high_water_ = next_seq;
    }

    // ── Gap handling ──────────────────────────────────────────────────────────

    // `first_known_after` is the lowest sequence known to exist past the
    // hole (the revealing packet's first seq, or a heartbeat's next seq).
    void handle_gap(uint64_t first_known_after, uint64_t now_ns) noexcept {
        if (in_gap_) return;   // already tracking this loss episode
        in_gap_             = true;
        gap_detected_at_ns_ = now_ns;
        gap_last_retx_ns_   = 0;
        ++stat_gaps_;

        if (on_gap_) {
            GapEvent ev;
            ev.first_missing_seq = next_expected_;
            ev.last_missing_seq  = first_known_after - 1;
            ev.detected_at_ns    = now_ns;
            on_gap_(ev);
        }
    }

    // After a delivery: is anything still missing between next_expected
    // and the high-water mark? If so the episode continues at the new
    // hole, with its original detection time.
    void update_gap_state(uint64_t now_ns) noexcept {
        if (next_expected_ >= high_water_) { in_gap_ = false; return; }
        if (!in_gap_) { handle_gap(first_present_after(), now_ns); return; }
        // Still in the same episode. If everything we last asked for has
        // now arrived, the remaining hole was simply outside that request
        // (a second hole, or a range beyond max_retransmit_count): ask for
        // it on the next check instead of waiting out retry_interval.
        if (gap_last_retx_ns_ != 0 && next_expected_ >= retx_requested_end_)
            gap_last_retx_ns_ = 0;
    }

    // Lowest sequence past next_expected_ that is buffered, or the
    // high-water mark if none is (bounded by the buffer window).
    uint64_t first_present_after() const noexcept {
        const uint64_t limit = std::min<uint64_t>(high_water_, next_expected_ + kCapacity);
        for (uint64_t s = next_expected_; s < limit; ++s) {
            const auto& slot = slots_[s & kMask];
            if (slot.occupied && slot.seq_num == s) return s;
        }
        return high_water_;
    }

    // Length of the contiguous missing run starting at next_expected_,
    // bounded by the high-water mark and the request cap.
    uint64_t missing_run() const noexcept {
        const uint64_t limit = std::min<uint64_t>(high_water_ - next_expected_,
                                                  cfg_.max_retransmit_count);
        uint64_t n = 0;
        for (uint64_t s = next_expected_; n < limit; ++s, ++n) {
            const auto& slot = slots_[s & kMask];
            if (slot.occupied && slot.seq_num == s) break;
        }
        return n;
    }

    void check_gap_timeout(uint64_t now_ns) noexcept {
        if (!in_gap_) return;

        const bool first_request = (gap_last_retx_ns_ == 0) &&
                                   (now_ns - gap_detected_at_ns_ >= cfg_.gap_timeout_ns);
        const bool retry_request = (gap_last_retx_ns_ != 0) &&
                                   (now_ns - gap_last_retx_ns_ >= cfg_.retry_interval_ns);
        if (!first_request && !retry_request) return;
        if (!on_retx_) return;

        const uint64_t missing = missing_run();
        if (missing == 0) { in_gap_ = (next_expected_ < high_water_); return; }

        RetransmitRequest req;
        std::memcpy(req.session, session_, 10);
        req.first_seq = next_expected_;
        req.count     = static_cast<uint16_t>(missing);
        on_retx_(req);

        gap_last_retx_ns_   = now_ns;
        retx_requested_end_ = next_expected_ + missing;
        ++stat_retx_;
    }

    // ── Message walk: deliver / buffer / skip ─────────────────────────────────

    int walk_packet(const MoldHeader& hdr, const uint8_t* payload,
                    std::size_t len, uint64_t recv_ns) noexcept
    {
        int count = 0;
        const uint8_t* cursor = payload + kMoldHeaderSize;
        const uint8_t* end    = payload + len;
        uint64_t seq = hdr.seq_num;

        for (uint16_t i = 0; i < hdr.msg_count; ++i, ++seq) {
            if (cursor + 2 > end) { ++stat_malformed_; break; }
            const uint16_t msg_len = be16(cursor);
            cursor += 2;
            if (cursor + msg_len > end) { ++stat_malformed_; break; }
            const uint8_t* body = cursor;
            cursor += msg_len;

            if (seq < next_expected_) continue;               // duplicate
            if (seq == next_expected_) {
                count += deliver(seq, body, msg_len, recv_ns);
                count += flush_buffer();
            } else {
                buffer_message(seq, body, msg_len, recv_ns);
            }
        }
        return count;
    }

    int deliver(uint64_t seq, const uint8_t* body, uint16_t len,
                uint64_t recv_ns) noexcept {
        // A copy of this seq may also be sitting in the buffer (it arrived
        // out of order on one line, then in order on another): release it.
        auto& slot = slots_[seq & kMask];
        if (slot.occupied && slot.seq_num == seq) slot.occupied = false;

        MoldMessage msg{ len, body, seq, recv_ns };
        if (on_message_) on_message_(msg);
        ++stat_delivered_;
        next_expected_ = seq + 1;
        return 1;
    }

    void buffer_message(uint64_t seq, const uint8_t* body, uint16_t len,
                        uint64_t recv_ns) noexcept {
        if (seq - next_expected_ >= kCapacity || len > kMaxMessageBytes) {
            ++stat_buffer_overflows_;
            return;   // still requestable later: high_water_ covers it
        }
        auto& slot = slots_[seq & kMask];
        if (slot.occupied && slot.seq_num == seq) return;     // already have it
        slot.occupied = true;
        slot.seq_num  = seq;
        slot.recv_ns  = recv_ns;
        slot.length   = len;
        std::memcpy(slot.data, body, len);
    }

    int flush_buffer() noexcept {
        int count = 0;
        for (;;) {
            auto& slot = slots_[next_expected_ & kMask];
            if (!slot.occupied || slot.seq_num != next_expected_) break;
            slot.occupied = false;
            MoldMessage msg{ slot.length, slot.data, slot.seq_num, slot.recv_ns };
            if (on_message_) on_message_(msg);
            ++stat_delivered_;
            ++next_expected_;
            ++count;
        }
        return count;
    }

    // ── State ─────────────────────────────────────────────────────────────────

    Config   cfg_;
    char     session_[10]    = {};
    bool     session_synced_ = false;
    bool     seq_synced_     = false;
    uint64_t next_expected_  = 0;
    uint64_t high_water_     = 0;

    // Gap tracking
    bool     in_gap_             = false;
    uint64_t gap_detected_at_ns_ = 0;
    uint64_t gap_last_retx_ns_   = 0;
    uint64_t retx_requested_end_ = 0;   // one past the last seq last requested

    // Session lifecycle
    bool     eos_seen_ = false;
    uint64_t eos_seq_  = 0;

    // Callbacks
    OnMessage           on_message_;
    OnGap               on_gap_;
    OnRetransmitRequest on_retx_;
    OnSessionChange     on_session_chg_;
    OnEndOfSession      on_eos_;

    // Out-of-order buffer, one slot per message sequence number.
    std::array<BufferedMessage, kCapacity> slots_;

    // Statistics
    uint64_t stat_packets_            = 0;
    uint64_t stat_messages_           = 0;
    uint64_t stat_gaps_               = 0;
    uint64_t stat_duplicates_         = 0;
    uint64_t stat_delivered_          = 0;
    uint64_t stat_retx_               = 0;
    uint64_t stat_heartbeats_         = 0;
    uint64_t stat_buffer_overflows_   = 0;
    uint64_t stat_session_mismatches_ = 0;
    uint64_t stat_session_changes_    = 0;
    uint64_t stat_malformed_          = 0;
};

} // namespace feed
