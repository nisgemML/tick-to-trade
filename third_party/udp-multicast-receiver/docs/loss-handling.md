# Loss Handling and Recovery

This documents what this receiver actually does when things go wrong, what
it deliberately leaves to the application layer, and what an operator
should do in each case. It's written as a runbook, not a feature list —
every section says what you'd see, what it means, and what to do about it.

---

## 1. The three UDP failure modes, and what handles each

| Failure | Detected by | Recovery |
|---|---|---|
| Packet loss | `GapBuffer`: `seq > next_expected` on a data packet **or a heartbeat** | Buffer + retransmit request (§2) |
| Tail loss (feed then goes quiet) | `GapBuffer`: heartbeat / End of Session `seq > next_expected` | Same as packet loss (§2) |
| Foreign / next-day session on the group | `GapBuffer`: session ≠ pinned session | Dropped + counted, or rollover (§2a) |
| Packet duplication | `GapBuffer`: `seq < next_expected` | Dropped silently, counted (`stat_duplicates`) |
| Packet reordering | `GapBuffer`: out-of-order buffer | Buffered, flushed in order once the gap fills |
| **Correlated loss on one line** | N/A for a single receiver | `FeedArbitrator` — see §3 |

None of this is new information if you've read `gap_buffer.hpp`'s header
comment — this section is about what happens *around* that logic, which is
where the actual operational risk lives.

---

## 2. Single-line gap recovery, end to end

1. A gap is detected: `on_gap` fires once, with the missing range.
2. The gap sits in `GapBuffer`'s buffer for `gap_timeout_ns` (library
   default 100µs; the `receiver` binary configures 200µs) — this is a debounce window, not a real recovery attempt: most
   gaps under ~100µs are reordering, not loss, and resolve themselves
   without a retransmit request ever being sent.
3. If still missing after the timeout, `on_retransmit` fires with a
   `RetransmitRequest` (session, first_seq, count). With a
   `RetransmitClient` attached (`MulticastReceiver::attach_retransmit_client`)
   it is sent as a 20-byte UDP datagram to the venue's MoldUDP64
   re-request server, and the server's replies — ordinary MoldUDP64
   packets, unicast back — are polled and ingested on the receive thread.
   The request covers the contiguous missing run at `next_expected`,
   bounded by the highest sequence the feed has evidenced (data,
   heartbeat or End of Session), so it never asks for sequences that
   don't exist yet. Without a client attached, `on_retransmit` is just a
   callback — wire it to whatever your venue needs.
4. If the gap is still open after `retry_interval_ns` (library default 1ms;
   the `receiver` binary configures 2ms), the
   request is resent — capped at `max_retransmit_count` (default 100)
   sequences per request, so a gap larger than that is requested in
   multiple rounds. Requests and replies are UDP and can be lost; that is
   handled by this same retry, not by any state in the client. Replies may
   also carry fewer messages than requested (one datagram's worth). When
   everything last requested has arrived but a hole remains (a second
   hole, or the rest of a capped range), the next request goes out on the
   next tick instead of waiting for the retry interval.
5. **Buffer overflow.** The out-of-order buffer is a fixed 4096-slot ring,
   one slot per message sequence (§ design note in `gap_buffer.hpp`). A
   message more than 4096 sequences past `next_expected` cannot be held;
   `stat_buffer_overflows` increments and it is dropped. Because the
   sequence high-water mark still advances, the dropped range is simply
   requested again once the window has moved: with a reachable
   re-request server, overflow costs extra retransmit traffic and
   latency, not data (`test_gap_buffer`'s `test_request_cap_and_overflow`
   recovers a gap larger than the window this way).

   **What to do:** without a re-request server, overflow is irrecoverable
   in-place — stop trusting the current `GapBuffer`/`OrderBook` state, tear
   both down, and reconnect (see §4 for what that needs to mean for the
   order book). With one, alert on a sustained overflow rate: it means the
   window is too small for your loss/latency profile.

## 2a. Heartbeats, End of Session and session identity

- **Heartbeat** (count 0): its sequence is the next sequence the server
  will send. A heartbeat ahead of `next_expected` opens a gap — the only
  way loss of the last packets before a quiet period is ever detected.
- **End of Session** (count 0xFFFF): same sequence rule, carries no
  messages. `OnEndOfSession` fires once; `session_complete()` becomes true
  when every message before it has been delivered. Missing messages
  before End of Session are still requested.
- **Session identity:** the 10-byte session is pinned from
  `Config::session` (if non-blank) or from the first packet. A packet for
  another session is dropped and counted (`stat_session_mismatches`) —
  stray traffic can never be mixed into this sequence space. With
  `accept_session_change = true` it is instead a **rollover**: all
  buffered state is discarded, sequencing resyncs to the new session,
  and `OnSessionChange` fires. Anything built from the old session (the
  order book above all) must be reset on that callback.
- **Where the stream starts:** by default the buffer syncs to the first
  packet it sees (a late join). Set `expected_first_seq = 1` when the
  receiver is up before session open: a lost *first* packet is then a
  gap and gets requested, instead of the session silently starting at 2.

---

## 3. A/B feed lines change the shape of §2, not the logic

`FeedArbitrator` doesn't add new recovery logic — it shares one
`GapBuffer` across two lines, so whichever line delivers sequence N first
wins and the other's copy is a free duplicate-drop (see
`feed_arbitrator.hpp` for why that's correct with zero new state
machinery). The operational consequence:

- A loss on **one** line that the other line covers **never reaches §2 at
  all** — no gap event, no retransmit request, because from `GapBuffer`'s
  perspective the sequence just arrived a little late from the other
  socket. This is the entire value of running both lines instead of one.
- A gap only fires — and only then does §2 apply — when **both** lines are
  missing the same range. That's a genuine outage, not a switch hiccup,
  and should be treated with more urgency than a single-line gap would be.
- **Monitoring implication:** if you only alert on `stat_gaps()` /
  `stat_retransmit_requests()`, you will not see a line going dark as long
  as the other line covers for it — which is correct for *data
  correctness* but means you can lose real redundancy silently. Alert
  separately on `FeedArbitrator::line_looks_dark()` per line; by the time
  a real dual-line gap fires, you've already been running on single-line
  redundancy for however long the dark line has been down.

---

## 4. Order book state is a second thing that can be "gapped" —
and it's easy to miss

This is the least obvious failure mode in the whole pipeline, and it's not
a `GapBuffer` problem at all — it's inherent to how ITCH represents book
state, and it matters for anyone extending this repo's order book.

`GapBuffer` guarantees the *message stream* the application sees is
gap-free and ordered. It says nothing about whether that message stream
started at the *beginning of the session*. ITCH has no "give me the full
current book" message — the book is defined entirely as the accumulated
effect of every Add/Cancel/Delete/Executed/Replace since session open. If
a consumer's `GapBuffer` starts syncing at, say, sequence 4,000,000
because that's simply when the process started, `OrderBook` will build a
completely gap-free, internally-consistent — and **wrong** — book: every
order added before sequence 4,000,000 is invisible, `Cancel`/`Delete`
messages referencing those pre-existing orders show up as
`stat_unknown_order` (harmless to the process, but silently wrong for the
book), and the reconstructed top-of-book can be meaningfully different
from the real one.

**This will not show up as an error anywhere in this codebase.** No gap
fires, no stat trips, nothing crashes. The book is just quietly wrong from
depth on down, and possibly wrong at the top too if resting liquidity from
before startup was sitting inside the current best price.

**What to do:**
- Start the receiver before the session opens (before the exchange's
  first Add message), so `next_expected` syncs to the true first
  sequence — this is the only way this codebase gives you a genuinely
  correct book with zero extra work.
- If you must start mid-session (a restart after a crash, a redeploy),
  you need one of: (a) a venue-provided snapshot mechanism outside ITCH
  itself, or (b) a full retransmit from sequence 1 via the re-request
  server described in §2 — `expected_first_seq = 1` makes `GapBuffer`
  request exactly that. It works (overflowed messages are re-requested as
  the window advances), but for a large session it is a bulk backfill at
  100 messages per request while live traffic overflows the window; a
  venue's dedicated replay/snapshot service is the better tool for that
  size of recovery.
- At minimum, log and monitor `next_expected()` at startup against a
  known session-open sequence number if your venue publishes one, so a
  mid-session cold start is a visible, alertable event rather than a
  silent one.

---

## 5. What's genuinely out of scope here (and why)

- **CPU pinning / `SCHED_FIFO`.** Belongs in the application's startup
  sequence, not the library — see `README.md`'s Production Considerations.
- **Venue-specific recovery extras.** `RetransmitClient` speaks plain
  MoldUDP64 re-request over UDP. Venue-specific rate limits, multiple
  request servers, and escalation when a server can no longer serve a
  range (it only keeps so much history) are left to the application.
- **Snapshot/refresh outside ITCH.** Not an ITCH concept at all — see §4.
- **DPDK / kernel bypass.** `GapBuffer::ingest()` takes a raw byte span
  and is transport-agnostic; swap the receive path, keep everything above
  it.

---

## 6. Testing this without a live feed

- `tools/gen_pcap` injects controlled gaps and duplicates into a synthetic
  PCAP file (`--gap-at`, `--gap-len`, `--dup-at`) — replay it through
  `receiver --replay` to exercise §2 end to end offline.
- `tests/test_gap_buffer.cpp` covers the gap/duplicate/reorder state
  machine directly, plus §2a: tail loss via heartbeat, End of Session,
  session pin/mismatch/rollover, retransmits re-packetised differently
  from the originals, multi-hole gaps, window overflow.
- `tests/test_retransmit.cpp` runs §2 over real UDP sockets on loopback
  against `feed/rerequest_server.hpp`: line loss, tail loss, lost replies
  and partial replies, all recovered in order; no server; wrong-session
  server.
- `tests/test_feed_arbitrator.cpp` covers §3: it specifically asserts that
  a one-line drop covered by the other line produces **zero** gap events
  and **zero** retransmit requests, and that a drop on both lines still
  produces exactly one real gap — i.e. it tests the *difference* single-vs
  dual-line makes, not just that arbitration runs without crashing.
- `tests/test_order_book.cpp`'s end-to-end test exercises §4's "book only
  sees what GapBuffer delivered" property directly, by checking that a
  cancel sitting behind an unfilled gap has visibly not been applied to
  the book until the gap closes.
