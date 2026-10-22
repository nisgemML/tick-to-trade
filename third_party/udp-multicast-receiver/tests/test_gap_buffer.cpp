// test_gap_buffer.cpp — Tests for sequence gap detection and out-of-order buffering.
//
// These tests validate the GapBuffer's core invariants:
//
//   1. In-order packets are delivered immediately, in sequence.
//   2. Out-of-order packets are buffered and flushed when the gap fills.
//   3. Duplicate packets (seq < next_expected) are dropped silently.
//   4. Gap detection fires exactly once per gap.
//   5. Retransmit requests are sent after gap_timeout_ns, with correct seq range.
//   6. Flush after retransmit: buffered packets after the gap deliver correctly.
//   7. Partial overlaps (retransmit starts before next_expected) are handled.
//   8. Heartbeats reveal tail loss; End of Session is not data.
//   9. Session pinning, mismatches, and opt-in session rollover.
//  10. Retransmits re-packetised differently from the original (mid-packet
//      overlap), multi-hole gaps, request bounding, window overflow.

#include "feed/gap_buffer.hpp"
#include "feed/wire_format.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <vector>

using namespace feed;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; } else { ++passed; } \
    } while(0)

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a complete MoldUDP64 datagram with a single dummy ITCH message.
// The ITCH message body is just `msg_body_byte` repeated `msg_len` times.
static std::vector<uint8_t> make_packet(const char* session, uint64_t seq,
                                         uint16_t msg_count, uint8_t msg_body_byte,
                                         uint16_t msg_len = 4)
{
    // MoldHeader(20) + [length(2) + body(msg_len)] * msg_count
    const std::size_t total = kMoldHeaderSize + msg_count * (2 + msg_len);
    std::vector<uint8_t> buf(total, 0);

    MoldHeader hdr{};
    std::memcpy(hdr.session, session, 10);
    hdr.seq_num   = seq;
    hdr.msg_count = msg_count;
    hdr.serialise(buf.data());

    uint8_t* p = buf.data() + kMoldHeaderSize;
    for (uint16_t i = 0; i < msg_count; ++i) {
        put_be16(p, msg_len);
        std::memset(p + 2, msg_body_byte, msg_len);
        p += 2 + msg_len;
    }
    return buf;
}

// Make a heartbeat packet. `next_seq` is the next sequence the server will
// send (MoldUDP64 spec); 0 means "no position information".
static std::vector<uint8_t> make_heartbeat(const char* session, uint64_t next_seq = 0,
                                           uint16_t count = kMoldHeartbeatCount) {
    std::vector<uint8_t> buf(kMoldHeaderSize);
    MoldHeader hdr{};
    std::memcpy(hdr.session, session, 10);
    hdr.seq_num   = next_seq;
    hdr.msg_count = count;
    hdr.serialise(buf.data());
    return buf;
}

// ── Test: in-order delivery ───────────────────────────────────────────────────

static void test_inorder_delivery() {
    GapBuffer gb;
    std::vector<uint64_t> received_seqs;
    gb.set_on_message([&](const MoldMessage& m) {
        received_seqs.push_back(m.seq_num);
    });

    const char* sess = "SESSIONA  ";
    uint64_t now = 1'000'000;

    for (uint64_t seq = 1; seq <= 5; ++seq) {
        auto pkt = make_packet(sess, seq, 1, uint8_t(seq));
        gb.ingest(pkt.data(), pkt.size(), now);
        now += 1000;
    }

    CHECK(received_seqs.size() == 5, "5 messages delivered");
    for (int i = 0; i < 5; ++i)
        CHECK(received_seqs[i] == uint64_t(i + 1), "in-order seq");
    CHECK(gb.stat_gaps() == 0, "no gaps");
    CHECK(gb.stat_duplicates() == 0, "no duplicates");
    CHECK(gb.next_expected() == 6, "next_expected == 6");
}

// ── Test: duplicate detection ─────────────────────────────────────────────────

static void test_duplicate_detection() {
    GapBuffer gb;
    int delivered = 0;
    gb.set_on_message([&](const MoldMessage&) { ++delivered; });

    const char* sess = "SESSIONA  ";
    uint64_t now = 1'000'000;

    // Send seq 1 and 2.
    auto p1 = make_packet(sess, 1, 1, 0xAA);
    auto p2 = make_packet(sess, 2, 1, 0xBB);
    gb.ingest(p1.data(), p1.size(), now++);
    gb.ingest(p2.data(), p2.size(), now++);

    // Re-send seq 1 (duplicate).
    gb.ingest(p1.data(), p1.size(), now++);

    CHECK(delivered == 2,                  "only 2 unique messages delivered");
    CHECK(gb.stat_duplicates() == 1,       "1 duplicate counted");
    CHECK(gb.next_expected() == 3,         "next_expected unaffected by duplicate");
}

// ── Test: gap detection fires once ────────────────────────────────────────────

static void test_gap_detection() {
    GapBuffer gb;
    std::vector<GapEvent> gaps;
    gb.set_on_gap([&](const GapEvent& g) { gaps.push_back(g); });

    const char* sess = "SESSIONA  ";
    uint64_t now = 1'000'000;

    // Seq 1 delivered.
    auto p1 = make_packet(sess, 1, 1, 0x01);
    gb.ingest(p1.data(), p1.size(), now);

    // Skip seq 2,3.  Send seq 4 — reveals a gap of 2.
    auto p4 = make_packet(sess, 4, 1, 0x04);
    gb.ingest(p4.data(), p4.size(), now + 1000);

    CHECK(gaps.size() == 1,              "exactly one gap event");
    CHECK(gaps[0].first_missing_seq == 2,"gap starts at seq 2");
    CHECK(gaps[0].last_missing_seq  == 3,"gap ends at seq 3");

    // Sending seq 5 should NOT fire another gap event (gap already known).
    auto p5 = make_packet(sess, 5, 1, 0x05);
    gb.ingest(p5.data(), p5.size(), now + 2000);
    CHECK(gaps.size() == 1, "no second gap event for same gap");
}

// ── Test: out-of-order buffering and flush ────────────────────────────────────

static void test_outoforder_flush() {
    GapBuffer gb;
    std::vector<uint64_t> delivered;
    gb.set_on_message([&](const MoldMessage& m) { delivered.push_back(m.seq_num); });

    const char* sess = "SESSIONA  ";
    uint64_t now = 1'000'000;

    // Send seq 1.
    auto p1 = make_packet(sess, 1, 1, 0x01);
    gb.ingest(p1.data(), p1.size(), now++);

    // Skip seq 2, send seq 3 and 4 (buffered).
    auto p3 = make_packet(sess, 3, 1, 0x03);
    auto p4 = make_packet(sess, 4, 1, 0x04);
    gb.ingest(p3.data(), p3.size(), now++);
    gb.ingest(p4.data(), p4.size(), now++);

    CHECK(delivered.size() == 1, "only seq 1 delivered — seq 3,4 buffered");

    // Now deliver seq 2 — should flush 3 and 4 as well.
    auto p2 = make_packet(sess, 2, 1, 0x02);
    gb.ingest(p2.data(), p2.size(), now++);

    CHECK(delivered.size() == 4, "all 4 messages delivered after gap fills");
    for (int i = 0; i < 4; ++i)
        CHECK(delivered[i] == uint64_t(i + 1), "delivery order correct");
}

// ── Test: retransmit request timing ───────────────────────────────────────────

static void test_retransmit_request_timing() {
    GapBuffer::Config cfg;
    cfg.gap_timeout_ns    = 500'000;   // 500 µs
    cfg.retry_interval_ns = 1'000'000; // 1 ms
    std::memcpy(cfg.session, "SESSIONA  ", 10);

    GapBuffer gb(cfg);
    std::vector<RetransmitRequest> retx;
    gb.set_on_retransmit([&](const RetransmitRequest& r) { retx.push_back(r); });

    const char* sess = "SESSIONA  ";
    const uint64_t t0 = 1'000'000'000ULL;

    // Seq 1 in order, then seq 3 revealing a gap at seq 2.
    auto p1 = make_packet(sess, 1, 1, 0x01);
    auto p3 = make_packet(sess, 3, 1, 0x03);
    gb.ingest(p1.data(), p1.size(), t0);
    gb.ingest(p3.data(), p3.size(), t0 + 100);

    // Tick before timeout — no retransmit yet.
    gb.tick(t0 + 400'000);
    CHECK(retx.empty(), "no retransmit before timeout");

    // Tick after timeout — first retransmit.
    gb.tick(t0 + 600'000);
    CHECK(retx.size() == 1,           "first retransmit sent");
    CHECK(retx[0].first_seq == 2,     "retransmit starts at missing seq 2");
    CHECK(retx[0].count == 1,         "retransmit count == 1 (only seq 2 missing)");

    // Tick before retry interval — no second retransmit.
    gb.tick(t0 + 1'100'000);
    CHECK(retx.size() == 1, "no second retransmit before retry interval");

    // Tick after retry interval — second retransmit.
    gb.tick(t0 + 1'700'000);
    CHECK(retx.size() == 2, "second retransmit after retry interval");
}

// ── Test: retransmit fills the gap ────────────────────────────────────────────

static void test_retransmit_fills_gap() {
    GapBuffer gb;
    std::vector<uint64_t> delivered;
    gb.set_on_message([&](const MoldMessage& m) { delivered.push_back(m.seq_num); });

    const char* sess = "SESSIONA  ";
    uint64_t now = 1'000'000;

    // seq 1, gap 2, seq 3.
    gb.ingest(make_packet(sess, 1, 1, 0x01).data(),
              make_packet(sess, 1, 1, 0x01).size(), now++);
    gb.ingest(make_packet(sess, 3, 1, 0x03).data(),
              make_packet(sess, 3, 1, 0x03).size(), now++);

    CHECK(delivered.size() == 1, "only seq 1 before retransmit");

    // Retransmit arrives: seq 2.
    auto p2 = make_packet(sess, 2, 1, 0x02);
    gb.ingest(p2.data(), p2.size(), now++);

    CHECK(delivered.size() == 3, "seq 2 and 3 delivered after retransmit");
    CHECK(delivered[1] == 2,     "seq 2 delivered");
    CHECK(delivered[2] == 3,     "seq 3 delivered");
    CHECK(!gb.in_gap(),          "gap cleared");
}

// ── Test: heartbeat handling ──────────────────────────────────────────────────

static void test_heartbeat() {
    GapBuffer gb;
    int delivered = 0;
    gb.set_on_message([&](const MoldMessage&) { ++delivered; });

    const char* sess = "SESSIONA  ";
    auto hb = make_heartbeat(sess);
    int r = gb.ingest(hb.data(), hb.size(), 1'000'000);
    CHECK(r == 0,       "heartbeat returns 0 delivered");
    CHECK(delivered == 0, "no messages from heartbeat");
    CHECK(gb.stat_heartbeats() == 1, "heartbeat counted");
}

// ── Test: multi-message packets ───────────────────────────────────────────────

static void test_multi_message_packet() {
    GapBuffer gb;
    std::vector<uint64_t> delivered;
    gb.set_on_message([&](const MoldMessage& m) { delivered.push_back(m.seq_num); });

    const char* sess = "SESSIONA  ";
    // One packet, 3 messages starting at seq 1.
    auto pkt = make_packet(sess, 1, 3, 0xAA, 4);
    gb.ingest(pkt.data(), pkt.size(), 1'000'000);

    CHECK(delivered.size() == 3, "3 messages from one packet");
    CHECK(delivered[0] == 1, "first message seq 1");
    CHECK(delivered[1] == 2, "second message seq 2");
    CHECK(delivered[2] == 3, "third message seq 3");
    CHECK(gb.next_expected() == 4, "next_expected == 4");
}

// ── Test: parse error handling ────────────────────────────────────────────────

static void test_parse_errors() {
    GapBuffer gb;
    // Too-short buffer should return -1.
    uint8_t tiny[5] = {0};
    int r = gb.ingest(tiny, 5, 1'000'000);
    CHECK(r == -1, "short buffer returns -1");
}

// ── Test: large gap spanning buffer ───────────────────────────────────────────

static void test_large_sequential_stream() {
    GapBuffer gb;
    uint64_t total_delivered = 0;
    gb.set_on_message([&](const MoldMessage&) { ++total_delivered; });

    const char* sess = "SESSIONA  ";
    // Send 10000 in-order messages in packets of 10 each.
    uint64_t seq = 1;
    for (int i = 0; i < 1000; ++i) {
        auto pkt = make_packet(sess, seq, 10, 0x55, 4);
        gb.ingest(pkt.data(), pkt.size(), uint64_t(i) * 1000);
        seq += 10;
    }
    CHECK(total_delivered == 10000, "10000 messages delivered in order");
    CHECK(gb.stat_gaps() == 0,      "no gaps in clean stream");
    CHECK(gb.next_expected() == 10001, "next_expected correct");
}


// ── Helpers for packets with distinct per-message payloads ───────────────────

// One packet carrying messages [seq, seq+count); message s's body is the
// 4-byte value s (big-endian), so delivery content can be checked.
static std::vector<uint8_t> make_seq_packet(const char* session, uint64_t seq,
                                            uint16_t count) {
    std::vector<uint8_t> buf(kMoldHeaderSize + count * (2 + 4), 0);
    MoldHeader hdr{};
    std::memcpy(hdr.session, session, 10);
    hdr.seq_num = seq; hdr.msg_count = count;
    hdr.serialise(buf.data());
    uint8_t* p = buf.data() + kMoldHeaderSize;
    for (uint16_t i = 0; i < count; ++i) {
        put_be16(p, 4); put_be32(p + 2, uint32_t(seq + i)); p += 6;
    }
    return buf;
}

static void put(GapBuffer& gb, const std::vector<uint8_t>& pkt, uint64_t now) {
    gb.ingest(pkt.data(), pkt.size(), now);
}

struct Collector {
    std::vector<uint64_t> seqs;
    bool content_ok = true;
    void attach(GapBuffer& gb) {
        gb.set_on_message([this](const MoldMessage& m) {
            seqs.push_back(m.seq_num);
            if (m.length != 4 || be32(m.body) != uint32_t(m.seq_num)) content_ok = false;
        });
    }
    bool contiguous_from(uint64_t first, uint64_t n) const {
        if (seqs.size() != n) return false;
        for (uint64_t i = 0; i < n; ++i) if (seqs[i] != first + i) return false;
        return content_ok;
    }
};

// ── Test: heartbeat reveals tail loss ─────────────────────────────────────────

static void test_heartbeat_reveals_tail_loss() {
    GapBuffer::Config cfg; cfg.gap_timeout_ns = 1000; cfg.retry_interval_ns = 5000;
    auto gb = std::make_unique<GapBuffer>(cfg);
    std::vector<GapEvent> gaps; std::vector<RetransmitRequest> retx;
    gb->set_on_gap([&](const GapEvent& g) { gaps.push_back(g); });
    gb->set_on_retransmit([&](const RetransmitRequest& r) { retx.push_back(r); });
    Collector c; c.attach(*gb);
    const char* sess = "SESSIONA  ";

    put(*gb, make_seq_packet(sess, 1, 3), 100);          // 1..3 delivered
    // 4..6 lost; the feed then goes quiet and only heartbeats arrive.
    put(*gb, make_heartbeat(sess, 7), 200);

    CHECK(gaps.size() == 1,                 "heartbeat with seq > next_expected opens a gap");
    CHECK(gaps[0].first_missing_seq == 4,   "tail gap starts at 4");
    CHECK(gaps[0].last_missing_seq  == 6,   "tail gap ends at 6");
    CHECK(gb->high_water() == 7,            "high-water follows the heartbeat");

    gb->tick(2000);
    CHECK(retx.size() == 1,                 "retransmit fires for a heartbeat-only gap");
    CHECK(retx[0].first_seq == 4 && retx[0].count == 3,
          "request is exactly the 3 missing messages, not max_retransmit_count");

    put(*gb, make_seq_packet(sess, 4, 3), 3000);          // retransmit reply
    CHECK(c.contiguous_from(1, 6),          "all 6 delivered in order after recovery");
    CHECK(!gb->in_gap(),                    "gap closed");

    // An up-to-date heartbeat is not a gap.
    put(*gb, make_heartbeat(sess, 7), 4000);
    CHECK(gb->stat_gaps() == 1 && !gb->in_gap(), "in-sync heartbeat opens nothing");
}

// ── Test: heartbeat before any data syncs the sequence ───────────────────────

static void test_heartbeat_first_syncs() {
    auto gb = std::make_unique<GapBuffer>();
    Collector c; c.attach(*gb);
    const char* sess = "SESSIONA  ";
    put(*gb, make_heartbeat(sess, 50), 100);   // join mid-session during a quiet period
    CHECK(gb->next_expected() == 50, "heartbeat seq syncs next_expected");
    put(*gb, make_seq_packet(sess, 50, 2), 200);
    CHECK(c.contiguous_from(50, 2) && gb->stat_gaps() == 0, "data after heartbeat sync is in order");
}

// ── Test: End of Session ──────────────────────────────────────────────────────

static void test_end_of_session() {
    GapBuffer::Config cfg; cfg.gap_timeout_ns = 1000;
    auto gb = std::make_unique<GapBuffer>(cfg);
    std::vector<EndOfSessionEvent> eos; std::vector<RetransmitRequest> retx;
    gb->set_on_end_of_session([&](const EndOfSessionEvent& e) { eos.push_back(e); });
    gb->set_on_retransmit([&](const RetransmitRequest& r) { retx.push_back(r); });
    Collector c; c.attach(*gb);
    const char* sess = "SESSIONA  ";

    put(*gb, make_seq_packet(sess, 1, 4), 100);           // 1..4
    // 5 lost, then End of Session with next seq 6 (sent repeatedly, as venues do).
    auto e = make_heartbeat(sess, 6, kMoldEndOfSessionCount);
    const int r = gb->ingest(e.data(), e.size(), 200);
    put(*gb, e, 300);

    CHECK(r == 0,                           "EOS delivers nothing");
    CHECK(eos.size() == 1,                  "EOS callback fires once");
    CHECK(eos[0].final_next_seq == 6,       "EOS reports final next seq");
    CHECK(gb->stat_messages() == 4,         "EOS is not counted as 65535 messages");
    CHECK(gb->stat_packets() == 1,          "EOS is not counted as a data packet");
    CHECK(gb->stat_heartbeats() == 0,       "EOS is not a heartbeat");
    CHECK(gb->end_of_session(),             "end_of_session() set");
    CHECK(!gb->session_complete(),          "not complete: seq 5 still missing");
    CHECK(gb->in_gap(),                     "EOS revealed the gap at 5");

    gb->tick(5000);
    CHECK(retx.size() == 1 && retx[0].first_seq == 5 && retx[0].count == 1,
          "missing pre-EOS message is still requested");
    put(*gb, make_seq_packet(sess, 5, 1), 6000);
    CHECK(c.contiguous_from(1, 5),          "all messages delivered");
    CHECK(gb->session_complete(),           "session complete once EOS + no gap");
}

// ── Test: session pinning and mismatches ─────────────────────────────────────

static void test_session_mismatch_dropped() {
    auto gb = std::make_unique<GapBuffer>();   // learns session from first packet
    Collector c; c.attach(*gb);
    put(*gb, make_seq_packet("SESSIONA  ", 1, 2), 100);
    // Stray traffic from another session on the same group: seq 1 again.
    put(*gb, make_seq_packet("SESSIONB  ", 1, 5), 200);
    put(*gb, make_heartbeat("SESSIONB  ", 900), 250);
    put(*gb, make_seq_packet("SESSIONA  ", 3, 1), 300);

    CHECK(c.contiguous_from(1, 3),            "only SESSIONA messages delivered");
    CHECK(gb->stat_session_mismatches() == 2, "both foreign packets counted");
    CHECK(gb->stat_gaps() == 0,               "foreign heartbeat did not open a gap");
    CHECK(gb->session() == "SESSIONA  ",      "session unchanged");
}

static void test_session_pinned_by_config() {
    GapBuffer::Config cfg; std::memcpy(cfg.session, "EXPECTED  ", 10);
    auto gb = std::make_unique<GapBuffer>(cfg);
    Collector c; c.attach(*gb);
    put(*gb, make_seq_packet("WRONGONE  ", 1, 3), 100);   // first packet, wrong session
    put(*gb, make_seq_packet("EXPECTED  ", 10, 2), 200);
    CHECK(gb->stat_session_mismatches() == 1, "first packet from wrong session rejected");
    CHECK(c.contiguous_from(10, 2),           "pinned session synced from its own first packet");
}

static void test_session_change_accepted() {
    GapBuffer::Config cfg; cfg.accept_session_change = true;
    auto gb = std::make_unique<GapBuffer>(cfg);
    std::vector<SessionChangeEvent> chg;
    gb->set_on_session_change([&](const SessionChangeEvent& e) { chg.push_back(e); });
    Collector c; c.attach(*gb);

    put(*gb, make_seq_packet("DAY1      ", 100, 2), 100);   // 100,101
    put(*gb, make_seq_packet("DAY1      ", 105, 1), 200);   // buffered behind a gap
    CHECK(gb->in_gap(), "gap open in old session");

    // Rollover: new session restarts at 1.
    put(*gb, make_seq_packet("DAY2      ", 1, 3), 300);
    CHECK(chg.size() == 1,                         "session change reported");
    CHECK(std::memcmp(chg[0].old_session, "DAY1      ", 10) == 0, "old session");
    CHECK(std::memcmp(chg[0].new_session, "DAY2      ", 10) == 0, "new session");
    CHECK(!gb->in_gap(),                           "old session's gap discarded");
    CHECK(gb->next_expected() == 4,                "resynced to new session");
    CHECK(c.seqs.size() == 5 && c.seqs[2] == 1 && c.seqs[4] == 3,
          "new session delivered from 1, old buffered seq 105 never leaks");
    put(*gb, make_seq_packet("DAY2      ", 4, 1), 400);
    CHECK(c.seqs.size() == 6 && c.seqs[5] == 4, "new session continues normally");
}

// ── Test: retransmit re-packetised differently (mid-packet overlap) ──────────

static void test_repacketised_retransmit_overlap() {
    auto gb = std::make_unique<GapBuffer>();
    Collector c; c.attach(*gb);
    const char* sess = "SESSIONA  ";

    put(*gb, make_seq_packet(sess, 1, 1), 100);    // 1
    put(*gb, make_seq_packet(sess, 5, 4), 200);    // 5..8 buffered; 2..4 missing
    // Replay of 2..6 in ONE packet: overlaps the buffered packet mid-way.
    put(*gb, make_seq_packet(sess, 2, 5), 300);

    CHECK(c.contiguous_from(1, 8),       "1..8 delivered exactly once, in order");
    CHECK(gb->next_expected() == 9,      "next_expected past the buffered tail");
    CHECK(!gb->in_gap(),                 "no phantom gap left behind");

    // And the reverse: replay ends inside a later buffered packet.
    put(*gb, make_seq_packet(sess, 12, 3), 400);   // 12..14 buffered
    put(*gb, make_seq_packet(sess, 9, 2), 500);    // 9..10, 11 still missing
    put(*gb, make_seq_packet(sess, 10, 4), 600);   // 10..13 overlap both sides
    CHECK(c.contiguous_from(1, 14),      "1..14 delivered exactly once, in order");
    CHECK(!gb->in_gap(),                 "gap closed");
}

// ── Test: multiple holes are all requested ───────────────────────────────────

static void test_multiple_holes() {
    GapBuffer::Config cfg; cfg.gap_timeout_ns = 1000; cfg.retry_interval_ns = 1'000'000;
    auto gb = std::make_unique<GapBuffer>(cfg);
    std::vector<RetransmitRequest> retx; int gap_events = 0;
    gb->set_on_retransmit([&](const RetransmitRequest& r) { retx.push_back(r); });
    gb->set_on_gap([&](const GapEvent&) { ++gap_events; });
    Collector c; c.attach(*gb);
    const char* sess = "SESSIONA  ";

    put(*gb, make_seq_packet(sess, 1, 1), 100);     // 1
    put(*gb, make_seq_packet(sess, 3, 2), 200);     // 3,4    (2 missing)
    put(*gb, make_seq_packet(sess, 7, 2), 300);     // 7,8    (5,6 missing)

    gb->tick(2000);
    CHECK(retx.size() == 1 && retx[0].first_seq == 2 && retx[0].count == 1,
          "first request covers the first hole only");
    put(*gb, make_seq_packet(sess, 2, 1), 3000);    // fills 2 → 3,4 flush; stops at 5
    CHECK(gb->in_gap(),                   "gap stays open at the second hole");
    CHECK(gb->next_expected() == 5,       "advanced to second hole");
    CHECK(gap_events == 1,                "same loss episode, not a new gap event");

    // Everything requested has arrived, so the second hole is requested
    // on the next check — not after the 1 ms retry interval.
    gb->tick(3100);
    CHECK(retx.size() == 2 && retx[1].first_seq == 5 && retx[1].count == 2,
          "second hole requested promptly");
    put(*gb, make_seq_packet(sess, 5, 2), 4000);
    CHECK(c.contiguous_from(1, 8) && !gb->in_gap(), "all holes filled");
}

// ── Test: partial reply does not trigger premature re-request ────────────────

static void test_partial_reply_waits() {
    GapBuffer::Config cfg; cfg.gap_timeout_ns = 1000; cfg.retry_interval_ns = 1'000'000;
    auto gb = std::make_unique<GapBuffer>(cfg);
    std::vector<RetransmitRequest> retx;
    gb->set_on_retransmit([&](const RetransmitRequest& r) { retx.push_back(r); });
    const char* sess = "SESSIONA  ";

    put(*gb, make_seq_packet(sess, 1, 1), 100);
    put(*gb, make_seq_packet(sess, 11, 1), 200);    // 2..10 missing
    gb->tick(2000);
    CHECK(retx.size() == 1 && retx[0].count == 9, "one request for 2..10");
    put(*gb, make_seq_packet(sess, 2, 4), 2100);    // first reply datagram: 2..5
    gb->tick(2200);
    CHECK(retx.size() == 1, "rest of the reply is in flight — no duplicate request yet");
    gb->tick(2000 + 1'000'001);
    CHECK(retx.size() == 2 && retx[1].first_seq == 6 && retx[1].count == 5,
          "after retry interval, re-request only what is still missing");
}

// ── Test: request size cap and window overflow recovery ──────────────────────

static void test_request_cap_and_overflow() {
    GapBuffer::Config cfg; cfg.gap_timeout_ns = 1000; cfg.max_retransmit_count = 100;
    auto gb = std::make_unique<GapBuffer>(cfg);
    std::vector<RetransmitRequest> retx;
    gb->set_on_retransmit([&](const RetransmitRequest& r) { retx.push_back(r); });
    Collector c; c.attach(*gb);
    const char* sess = "SESSIONA  ";

    put(*gb, make_seq_packet(sess, 1, 1), 100);
    // A packet far beyond the 4096-message window: cannot be buffered.
    const uint64_t far = 1 + GapBuffer::kCapacity + 10;
    put(*gb, make_seq_packet(sess, far, 1), 200);
    CHECK(gb->stat_buffer_overflows() == 1, "beyond-window message counted as overflow");
    CHECK(gb->high_water() == far + 1,      "high-water still advanced");

    gb->tick(2000);
    CHECK(retx.size() == 1 && retx[0].first_seq == 2 && retx[0].count == 100,
          "request capped at max_retransmit_count");

    // Replay the whole range in max-sized chunks, as a server would.
    uint64_t t = 3000;
    while (gb->next_expected() <= far) {
        const uint64_t s = gb->next_expected();
        const uint16_t n = uint16_t(std::min<uint64_t>(100, far + 1 - s));
        put(*gb, make_seq_packet(sess, s, n), t++);
    }
    CHECK(c.contiguous_from(1, far),        "overflowed range recovered via retransmit");
    CHECK(!gb->in_gap(),                    "no gap left");
}

// ── Test: truncated packet ───────────────────────────────────────────────────

static void test_truncated_packet() {
    auto gb = std::make_unique<GapBuffer>();
    Collector c; c.attach(*gb);
    const char* sess = "SESSIONA  ";
    auto pkt = make_seq_packet(sess, 1, 3);
    pkt.resize(pkt.size() - 3);                 // cut into message 3
    put(*gb, pkt, 100);
    CHECK(c.contiguous_from(1, 2),             "complete messages delivered");
    CHECK(gb->stat_malformed() == 1,           "truncation counted");
    CHECK(gb->in_gap() && gb->next_expected() == 3, "lost message 3 is a gap, not silently skipped");
}

// ── Test: expected_first_seq recovers a lost first packet ────────────────────

static void test_expected_first_seq() {
    GapBuffer::Config cfg; cfg.gap_timeout_ns = 1000; cfg.expected_first_seq = 1;
    auto gb = std::make_unique<GapBuffer>(cfg);
    std::vector<RetransmitRequest> retx;
    gb->set_on_retransmit([&](const RetransmitRequest& r) { retx.push_back(r); });
    Collector c; c.attach(*gb);
    const char* sess = "SESSIONA  ";

    put(*gb, make_seq_packet(sess, 4, 2), 100);      // 1..3 never arrived
    CHECK(gb->in_gap() && gb->next_expected() == 1, "loss before first packet is a gap");
    gb->tick(5000);
    CHECK(retx.size() == 1 && retx[0].first_seq == 1 && retx[0].count == 3,
          "requests from the start of the session");
    put(*gb, make_seq_packet(sess, 1, 3), 6000);
    CHECK(c.contiguous_from(1, 5), "full session delivered from seq 1");

    // Default (0): late join syncs to whatever arrives first.
    auto gb2 = std::make_unique<GapBuffer>();
    put(*gb2, make_seq_packet(sess, 4, 2), 100);
    CHECK(!gb2->in_gap() && gb2->next_expected() == 6, "default: sync to first packet");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== Gap Buffer Tests ===\n\n");
    test_inorder_delivery();
    test_duplicate_detection();
    test_gap_detection();
    test_outoforder_flush();
    test_retransmit_request_timing();
    test_retransmit_fills_gap();
    test_heartbeat();
    test_multi_message_packet();
    test_parse_errors();
    test_large_sequential_stream();
    test_heartbeat_reveals_tail_loss();
    test_heartbeat_first_syncs();
    test_end_of_session();
    test_session_mismatch_dropped();
    test_session_pinned_by_config();
    test_session_change_accepted();
    test_repacketised_retransmit_overlap();
    test_multiple_holes();
    test_partial_reply_waits();
    test_request_cap_and_overflow();
    test_truncated_packet();
    test_expected_first_seq();

    std::printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
