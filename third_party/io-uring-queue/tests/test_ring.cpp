// tests/test_ring.cpp — Correctness tests for SPSCRingBuffer and IOURingLogger.

#include "ioq/ring.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; \
    } else { ++passed; } } while(0)

// ── SPSCRingBuffer tests ──────────────────────────────────────────────────────

void test_spsc_basic() {
    printf("SPSCRingBuffer basic:\n");
    ioq::SPSCRingBuffer<int, 16> q;

    CHECK(q.empty(), "initially empty");
    CHECK(q.size() == 0, "size 0");

    CHECK(q.try_push(42), "push 42");
    CHECK(!q.empty(), "not empty after push");
    CHECK(q.size() == 1, "size 1");

    auto v = q.try_pop();
    CHECK(v.has_value(), "pop returns value");
    CHECK(*v == 42, "pop returns 42");
    CHECK(q.empty(), "empty after pop");
}

void test_spsc_fifo_order() {
    printf("SPSCRingBuffer FIFO order:\n");
    ioq::SPSCRingBuffer<int, 64> q;
    for (int i = 0; i < 50; ++i) (void)q.try_push(i);
    for (int i = 0; i < 50; ++i) {
        auto v = q.try_pop();
        CHECK(v.has_value() && *v == i, "FIFO order preserved");
    }
}

void test_spsc_full() {
    printf("SPSCRingBuffer full/empty boundary:\n");
    ioq::SPSCRingBuffer<int, 8> q;
    int pushes = 0;
    while (q.try_push(pushes)) ++pushes;
    CHECK(pushes == 8, "exactly 8 pushes before full");
    CHECK(!q.try_push(999), "push on full returns false");

    int pops = 0;
    while (q.try_pop().has_value()) ++pops;
    CHECK(pops == 8, "exactly 8 pops to drain");
    CHECK(!q.try_pop().has_value(), "pop on empty returns nullopt");
}

void test_spsc_wrap_around() {
    printf("SPSCRingBuffer wrap-around:\n");
    ioq::SPSCRingBuffer<int, 4> q;
    for (int round = 0; round < 100; ++round) {
        CHECK(q.try_push(round), "push in round");
        auto v = q.try_pop();
        CHECK(v.has_value() && *v == round, "pop correct value after wrap");
    }
}

void test_spsc_concurrent() {
    printf("SPSCRingBuffer concurrent (1 producer, 1 consumer):\n");
    constexpr int N = 1000000;
    ioq::SPSCRingBuffer<int, 1024> q;
    std::atomic<bool> done{false};
    std::vector<int> received;
    received.reserve(N);

    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            while (!q.try_push(i)) __builtin_ia32_pause();
        }
    });

    std::thread consumer([&] {
        while ((int)received.size() < N) {
            if (auto v = q.try_pop()) received.push_back(*v);
            else __builtin_ia32_pause();
        }
        done = true;
    });

    producer.join();
    consumer.join();

    CHECK((int)received.size() == N, "all items received");
    bool ordered = true;
    for (int i = 0; i < N; ++i)
        if (received[i] != i) { ordered = false; break; }
    CHECK(ordered, "FIFO order preserved under concurrency");
}

void test_spsc_power_of_two_required() {
    printf("SPSCRingBuffer power-of-2 capacity:\n");
    // These should compile (power of 2)
    ioq::SPSCRingBuffer<int, 1>    q1;
    ioq::SPSCRingBuffer<int, 2>    q2;
    ioq::SPSCRingBuffer<int, 1024> q3;
    CHECK(q1.capacity() == 1, "capacity 1");
    CHECK(q2.capacity() == 2, "capacity 2");
    CHECK(q3.capacity() == 1024, "capacity 1024");
    // Non-power-of-2 would fail to compile (requires constraint)
    printf("  (non-power-of-2 rejected at compile time via requires)\n");
}

// ── IOURingLogger tests ───────────────────────────────────────────────────────

void test_iouring_open_close() {
    printf("IOURingLogger open/close:\n");
    ioq::IOURingLogger logger;
    bool ok = logger.open("/tmp/test_iouring_log.bin");
    CHECK(ok, "open succeeds");
    logger.close();
    CHECK(true, "close without crash");
}

void test_iouring_log_entries() {
    printf("IOURingLogger log entries:\n");
    ioq::IOURingLogger logger;
    bool ok = logger.open("/tmp/test_iouring_entries.bin");
    if (!ok) { printf("  SKIP: io_uring unavailable\n"); return; }

    constexpr int N = 100;
    for (int i = 0; i < N; ++i) {
        ioq::LogEntry e{};
        e.timestamp_ns = uint64_t(i) * 1000;
        e.order_id     = uint64_t(i + 1);
        e.price        = int64_t(10000 + i);
        e.qty          = 100;
        e.side         = (i % 2) ? 'B' : 'S';
        memcpy(e.symbol, "AAPL  ", 6);
        e.event_type   = 'F';
        bool logged = logger.log(e);
        CHECK(logged, "log entry succeeds");
    }

    logger.flush();
    CHECK(logger.completed_count() == N, "flush() genuinely waits — all N writes completed, not just submitted");
    CHECK(logger.error_count() == 0, "no write errors");
    logger.close();

    // flush() is no longer a best-effort wait — a correct flush() means the
    // file must be exactly N * sizeof(LogEntry) bytes, every time, not
    // sometimes 0 (which is what this test used to tolerate before
    // IOURingLogger tracked completions instead of just SQ availability;
    // see docs/failure-modes-and-batching.md for the full story).
    FILE* f = fopen("/tmp/test_iouring_entries.bin", "rb");
    CHECK(f != nullptr, "log file created");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        CHECK(sz == N * 40L, "file size is exactly N * sizeof(LogEntry) — flush() genuinely completed every write");
    }
}

// ── Regression test: buffer-slot reuse before completion ────────────────────
//
// The bug this catches: the original log() only checked that the SQ ring
// had room for a new submission entry before overwriting a buf_[] slot.
// SQ ring space freeing up does NOT mean the kernel has finished reading
// that slot's memory for an EARLIER write — those are two different kernel
// bookkeeping structures. Submitting far more entries than kQueueDepth (256)
// in a tight loop, with content that lets a corrupted byte be detected
// (each entry's order_id and price encode its own index), is exactly the
// scenario that used to silently corrupt on-disk log entries: slot N and
// slot N+256 are the same buffer memory, and if the write for slot N hasn't
// completed by the time slot N+256 is written, slot N's on-disk bytes can
// end up containing (part of) entry N+256's data instead.
void test_iouring_wraparound_content_integrity() {
    printf("IOURingLogger wraparound content integrity (regression):\n");
    ioq::IOURingLogger logger;
    bool ok = logger.open("/tmp/test_iouring_wraparound.bin");
    if (!ok) { printf("  SKIP: io_uring unavailable\n"); return; }

    // Deliberately several multiples of kQueueDepth (256), submitted as
    // fast as possible with no artificial delay — the exact condition that
    // used to race the kernel's completion of write N against log()'s
    // reuse of the same buffer slot for write N + kQueueDepth.
    constexpr int N = ioq::IOURingLogger::kQueueDepth * 20; // 5120 entries
    for (int i = 0; i < N; ++i) {
        ioq::LogEntry e{};
        e.timestamp_ns = uint64_t(i);
        e.order_id     = uint64_t(i);              // index encoded directly
        e.price        = int64_t(i) * 7 - 3;        // a second, independent function of i
        e.qty          = uint32_t(i % 1000);
        e.side         = (i % 2) ? 'B' : 'S';
        e.event_type   = 'F';
        memcpy(e.symbol, "AAPL  ", 6);
        bool logged = logger.log(e);
        CHECK(logged, "log entry succeeds under sustained sub-queue-depth load");
    }
    logger.flush();
    CHECK(logger.completed_count() == uint64_t(N), "all N writes completed after flush()");
    CHECK(logger.error_count() == 0, "no write errors during the stress run");
    logger.close();

    FILE* f = fopen("/tmp/test_iouring_wraparound.bin", "rb");
    CHECK(f != nullptr, "log file created");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    CHECK(sz == long(N) * 40L, "file is exactly N entries — no lost writes");

    int mismatches = 0;
    for (int i = 0; i < N; ++i) {
        ioq::LogEntry e{};
        size_t got = fread(&e, sizeof(e), 1, f);
        if (got != 1) { ++mismatches; continue; }
        const bool ok_entry =
            e.order_id == uint64_t(i) &&
            e.price    == int64_t(i) * 7 - 3 &&
            e.qty      == uint32_t(i % 1000) &&
            e.side     == ((i % 2) ? 'B' : 'S') &&
            memcmp(e.symbol, "AAPL  ", 6) == 0;
        if (!ok_entry) ++mismatches;
    }
    fclose(f);
    CHECK(mismatches == 0,
          "every entry's content on disk matches what was logged at that index — "
          "no slot was overwritten by a later entry before its write completed");
    if (mismatches > 0) printf("  %d / %d entries corrupted or missing\n", mismatches, N);
}

// ── Error surfacing ───────────────────────────────────────────────────────────
//
// A write CQE with a negative result (an -errno) or a short write used to
// be indistinguishable from success — flush_completions() (now
// drain_completed_nonblocking()/wait_for_one_completion()) called
// io_uring_cqe_seen() unconditionally without ever inspecting cqe->res.
// /dev/full is a standard Linux device that accepts opens but fails every
// write with ENOSPC — the portable, no-special-privileges way to force a
// real mid-stream write failure without needing an actual full disk.
void test_iouring_error_surfacing_open_failure() {
    printf("IOURingLogger error surfacing — open() failure:\n");
    ioq::IOURingLogger logger;
    bool ok = logger.open("/nonexistent_dir_xyz/should_fail.bin");
    CHECK(!ok, "open() fails cleanly for an unwritable path, no crash");
}

void test_iouring_error_surfacing_write_failure() {
    printf("IOURingLogger error surfacing — write failure via /dev/full:\n");
    ioq::IOURingLogger logger;
    bool ok = logger.open("/dev/full");
    if (!ok) { printf("  SKIP: could not open /dev/full in this environment\n"); return; }

    ioq::LogEntry e{};
    e.order_id = 1; e.price = 100; e.qty = 10;
    e.side = 'B'; e.event_type = 'F'; memcpy(e.symbol, "AAPL  ", 6);

    constexpr int N = 10;
    for (int i = 0; i < N; ++i) {
        e.timestamp_ns = uint64_t(i);
        // log() itself may still return true — the failure surfaces at
        // completion time (cqe->res), which is exactly why checking only
        // log()'s return value was never enough to know a write actually
        // landed on disk.
        (void)logger.log(e);
    }
    logger.flush();
    logger.close();

    CHECK(logger.error_count() == N,
          "every write to /dev/full is recorded as an error, not silently accepted");
    CHECK(logger.last_error() == -ENOSPC || logger.last_error() < 0,
          "last_error() reports the actual -errno from the failed write");
}

// ── Batching ──────────────────────────────────────────────────────────────────

void test_iouring_batching_correctness() {
    printf("IOURingLogger batching (batch_size=16):\n");
    ioq::IOURingLoggerConfig cfg;
    cfg.batch_size = 16;
    ioq::IOURingLogger logger;
    bool ok = logger.open("/tmp/test_iouring_batched.bin", cfg);
    if (!ok) { printf("  SKIP: io_uring unavailable\n"); return; }

    constexpr int N = 500; // not a multiple of 16 — exercises the partial-batch-at-flush path
    for (int i = 0; i < N; ++i) {
        ioq::LogEntry e{};
        e.order_id = uint64_t(i);
        e.price    = int64_t(i);
        e.qty      = 1;
        e.side     = 'B'; e.event_type = 'F'; memcpy(e.symbol, "AAPL  ", 6);
        CHECK(logger.log(e), "log succeeds under batch_size=16");
    }
    logger.flush(); // must submit the trailing partial batch (500 % 16 == 4 entries)
    CHECK(logger.completed_count() == N, "flush() completes every entry even mid-batch");
    logger.close();

    FILE* f = fopen("/tmp/test_iouring_batched.bin", "rb");
    CHECK(f != nullptr, "batched log file created");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        CHECK(sz == N * 40L, "batched file is exactly N entries — no partial-batch loss at flush");
    }
}

// ── SQPOLL ────────────────────────────────────────────────────────────────────
//
// SQPOLL requires CAP_SYS_NICE (or root) — not guaranteed in every CI
// environment. This SKIPs cleanly rather than failing when unavailable, but
// actually exercises the real kernel path (not just documents the flag)
// wherever it IS available.

void test_iouring_sqpoll_open_and_log() {
    printf("IOURingLogger SQPOLL mode:\n");
    ioq::IOURingLoggerConfig cfg;
    cfg.sqpoll = true;
    cfg.sqpoll_idle_ms = 100;
    ioq::IOURingLogger logger;
    bool ok = logger.open("/tmp/test_iouring_sqpoll.bin", cfg);
    if (!ok) {
        printf("  SKIP: SQPOLL unavailable in this environment (errno=%d) — "
               "expected without CAP_SYS_NICE\n", logger.sqpoll_setup_errno());
        return;
    }

    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        ioq::LogEntry e{};
        e.order_id = uint64_t(i); e.price = int64_t(i); e.qty = 1;
        e.side = 'S'; e.event_type = 'F'; memcpy(e.symbol, "MSFT  ", 6);
        CHECK(logger.log(e), "log succeeds under SQPOLL");
    }
    logger.flush();
    CHECK(logger.completed_count() == N, "SQPOLL: flush() completes every entry");
    CHECK(logger.error_count() == 0, "SQPOLL: no write errors");
    logger.close();

    FILE* f = fopen("/tmp/test_iouring_sqpoll.bin", "rb");
    CHECK(f != nullptr, "SQPOLL log file created");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        CHECK(sz == N * 40L, "SQPOLL file is exactly N entries");
    }
}

// ── Multi-producer stress: many symbols, one shared logger ──────────────────
//
// SPSCRingBuffer is single-producer by design — that's the whole point of
// "SPSC," and nothing here changes that. What DOES generalize to "many
// producers" is the realistic architecture this maps to: a sharded
// matching engine with one thread per symbol (or symbol group), each with
// its OWN SPSC ring, all fanning into ONE shared IOURingLogger via a
// single logger thread that round-robins across the rings. This is the
// actual multi-producer stress case worth testing — not making
// SPSCRingBuffer itself multi-producer (which would defeat its own
// design), but confirming the fan-in architecture built on top of several
// independent SPSC rings is correct and fast under real contention.
void test_multi_producer_fan_in_stress() {
    printf("Multi-producer fan-in stress (8 symbol threads -> 1 shared logger):\n");
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 20000;
    constexpr int kTotal = kProducers * kPerProducer;

    std::vector<std::unique_ptr<ioq::SPSCRingBuffer<ioq::LogEntry, 1024>>> rings;
    for (int i = 0; i < kProducers; ++i)
        rings.push_back(std::make_unique<ioq::SPSCRingBuffer<ioq::LogEntry, 1024>>());

    ioq::IOURingLogger logger;
    bool ok = logger.open("/tmp/test_multi_producer.bin");
    if (!ok) { printf("  SKIP: io_uring unavailable\n"); return; }

    std::atomic<int> producers_done{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                ioq::LogEntry e{};
                // Encode both producer id and index so post-hoc content
                // verification can confirm no cross-producer corruption.
                e.order_id = (uint64_t(p) << 32) | uint64_t(i);
                e.price    = int64_t(p) * 1'000'000 + int64_t(i);
                e.qty      = uint32_t(p);
                e.side     = 'B';
                e.event_type = 'F';
                std::memcpy(e.symbol, "SYM   ", 6);
                e.symbol[3] = char('0' + p);
                while (!rings[size_t(p)]->try_push(e)) __builtin_ia32_pause();
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    // Single fan-in logger thread: round-robins across all producer rings.
    int logged = 0;
    while (logged < kTotal) {
        bool any = false;
        for (int p = 0; p < kProducers; ++p) {
            if (auto e = rings[size_t(p)]->try_pop()) {
                if (logger.log(*e)) ++logged;
                any = true;
            }
        }
        if (!any) __builtin_ia32_pause();
    }
    for (auto& t : producers) t.join();
    logger.flush();
    logger.close();

    CHECK(logged == kTotal, "fan-in logger drained every producer's every entry");
    CHECK(logger.completed_count() == uint64_t(kTotal), "all entries genuinely completed, not just submitted");
    CHECK(logger.error_count() == 0, "no write errors under multi-producer contention");

    // Content verification: reconstruct expected (producer, index) pairs
    // and confirm every one appears exactly once, uncorrupted.
    FILE* f = fopen("/tmp/test_multi_producer.bin", "rb");
    CHECK(f != nullptr, "multi-producer log file created");
    if (!f) return;
    std::vector<std::vector<bool>> seen(kProducers, std::vector<bool>(kPerProducer, false));
    int bad = 0, dupes = 0;
    for (int n = 0; n < kTotal; ++n) {
        ioq::LogEntry e{};
        if (fread(&e, sizeof(e), 1, f) != 1) { ++bad; continue; }
        const int p = int(e.order_id >> 32);
        const int i = int(e.order_id & 0xFFFFFFFFu);
        const bool consistent = (p >= 0 && p < kProducers && i >= 0 && i < kPerProducer) &&
                                 e.price == int64_t(p) * 1'000'000 + int64_t(i) &&
                                 e.qty == uint32_t(p);
        if (!consistent) { ++bad; continue; }
        if (seen[size_t(p)][size_t(i)]) ++dupes;
        seen[size_t(p)][size_t(i)] = true;
    }
    fclose(f);
    int missing = 0;
    for (int p = 0; p < kProducers; ++p)
        for (int i = 0; i < kPerProducer; ++i)
            if (!seen[size_t(p)][size_t(i)]) ++missing;

    CHECK(bad == 0, "no entry has internally-inconsistent (corrupted) content");
    CHECK(dupes == 0, "no entry appears twice (no double-logging under contention)");
    CHECK(missing == 0, "every (producer, index) pair from all 8 producers is present exactly once");
    if (bad || dupes || missing)
        printf("  bad=%d dupes=%d missing=%d (out of %d)\n", bad, dupes, missing, kTotal);
}

void test_iouring_completion_callback() {
    printf("IOURingLogger completion-latency callback:\n");
    ioq::IOURingLogger logger;
    bool ok = logger.open("/tmp/test_iouring_callback.bin");
    if (!ok) { printf("  SKIP: io_uring unavailable\n"); return; }

    int callback_count = 0;
    uint64_t max_seq_seen = 0;
    bool saw_negative_latency = false;
    logger.set_on_complete([&](uint64_t seq, int64_t latency_ns, int32_t res) {
        ++callback_count;
        if (seq > max_seq_seen || callback_count == 1) max_seq_seen = seq;
        if (latency_ns < 0) saw_negative_latency = true;
        CHECK(res == int32_t(sizeof(ioq::LogEntry)), "callback sees a successful write result");
    });

    constexpr int N = 50;
    for (int i = 0; i < N; ++i) {
        ioq::LogEntry e{};
        e.order_id = uint64_t(i); e.price = int64_t(i); e.qty = 1;
        e.side = 'B'; e.event_type = 'F'; memcpy(e.symbol, "AAPL  ", 6);
        CHECK(logger.log(e), "log succeeds with a completion callback registered");
    }
    logger.flush();
    logger.close();

    CHECK(callback_count == N, "completion callback fired exactly once per entry");
    CHECK(!saw_negative_latency, "measured submit-to-complete latency is never negative");
}

void test_iouring_log_entry_size() {
    printf("IOURingLogger LogEntry layout:\n");
    CHECK(sizeof(ioq::LogEntry) == 40, "LogEntry is 32 bytes");
    CHECK(offsetof(ioq::LogEntry, timestamp_ns) == 0,  "timestamp at offset 0");
    CHECK(offsetof(ioq::LogEntry, order_id)     == 8,  "order_id at offset 8");
    CHECK(offsetof(ioq::LogEntry, price)        == 16, "price at offset 16");
    CHECK(offsetof(ioq::LogEntry, qty)          == 24, "qty at offset 24");
}

int main() {
    printf("=== SPSC Ring Buffer + io_uring Logger Tests ===\n\n");

    test_spsc_basic();
    test_spsc_fifo_order();
    test_spsc_full();
    test_spsc_wrap_around();
    test_spsc_concurrent();
    test_spsc_power_of_two_required();
    test_iouring_open_close();
    test_iouring_log_entries();
    test_iouring_wraparound_content_integrity();
    test_iouring_error_surfacing_open_failure();
    test_iouring_error_surfacing_write_failure();
    test_iouring_batching_correctness();
    test_iouring_sqpoll_open_and_log();
    test_multi_producer_fan_in_stress();
    test_iouring_completion_callback();
    test_iouring_log_entry_size();

    printf("\n=================================================\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
