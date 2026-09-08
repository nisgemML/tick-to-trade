// examples/matching_engine_demo.cpp — a real end-to-end use case, not a
// microbenchmark: a simulated matching engine thread generating execution
// reports at a sustained rate, feeding a logger over three different
// architectures, measuring what actually matters — the matching engine
// THREAD's own per-report latency, which is the thing this whole repo
// exists to protect.
//
// ── Three modes, same workload, same machine ─────────────────────────────────
//
//   1. naive-sync   : matching engine thread calls ::write() directly,
//                      per report — no ring, no second thread. This is the
//                      "standard approach (bad)" from README.md, and until
//                      this file existed, this repo never actually measured
//                      it — only asserted its cost as "500-5,000 ns" from
//                      general knowledge.
//   2. ring+classic : matching engine thread pushes into the SPSC ring;
//                      a dedicated logger thread drains it and calls a
//                      blocking ::write() per report (ClassicLogger).
//   3. ring+iouring : same ring and logger thread, but the logger thread
//                      submits via IOURingLogger instead of write().
//
// Modes 2 and 3 share the exact same ring buffer type, drain-thread
// pattern, and report generation — the only variable that changes is the
// logger thread's I/O backend. This isolates "is io_uring worth it,
// given you've already decoupled with a ring" as its own question,
// separate from "should you decouple with a ring at all" (modes 1 vs 2/3
// answer that instead).
//
// ── What "measured impact" means here ─────────────────────────────────────
//
// The metric that matters is the MATCHING ENGINE THREAD's per-report
// latency — not the logger thread's, and not throughput. A matching engine
// that logs synchronously pays the I/O cost directly, on its own critical
// path, every single report. A matching engine with a ring in front of the
// logger pays only the ring push cost — the I/O cost still exists, but it's
// been moved to a thread the matching engine doesn't wait on. This program
// measures exactly that difference, on this machine, with this workload —
// not a documented estimate.

#include "ioq/ring.hpp"
#include "ioq/classic_logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>

using namespace ioq;

namespace {

// Raw-sample histogram — deliberately not bucketed. A fixed-range bucketed
// histogram silently clips anything past its range into the last bucket,
// which is exactly the kind of thing that looks like a working histogram
// right up until a real completion-latency tail (hundreds of microseconds,
// easily, for actual disk I/O under a shared/virtualized environment) blows
// past it and corrupts every percentile above whatever clipped. Storing
// raw samples and sorting is slower to compute percentiles from but cannot
// silently lie about the range — 200k int64_t samples is ~1.6MB, a
// non-issue at this scale.
struct Hist {
    std::vector<int64_t> v;
    void rec(int64_t x) { v.push_back(x); }
    uint64_t total() const { return v.size(); }
    double mean() const {
        if (v.empty()) return 0.0;
        long double sum = 0; for (auto x : v) sum += x;
        return double(sum / (long double)v.size());
    }
    // Sorts in place — call once after all rec() calls, before any pct().
    void finalize() { std::sort(v.begin(), v.end()); }
    int64_t pct(double p) const {
        if (v.empty()) return 0;
        size_t idx = size_t(p * double(v.size() - 1));
        return v[idx];
    }
    int64_t max() const { return v.empty() ? 0 : v.back(); }
};

uint64_t monotonic_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
}

LogEntry make_report(uint64_t i) {
    LogEntry e{};
    e.timestamp_ns = monotonic_ns();
    e.order_id     = i;
    e.price        = int64_t(150000 + (i % 500));
    e.qty          = uint32_t(100 + (i % 900));
    e.side         = (i % 2) ? 'B' : 'S';
    e.event_type   = 'F';
    std::memcpy(e.symbol, "AAPL  ", 6);
    return e;
}

void print_hist(const char* label, Hist& h) {
    h.finalize();
    std::printf("%-28s n=%-8lu mean=%8.0fns  p50=%6lldns  p99=%7lldns  p99.9=%8lldns  max=%8lldns\n",
                label, (unsigned long)h.total(), h.mean(),
                (long long)h.pct(0.50), (long long)h.pct(0.99),
                (long long)h.pct(0.999), (long long)h.max());
}

constexpr int kReports = 200'000;

// ── Mode 1: naive synchronous write() on the matching engine thread ─────────

Hist run_naive_sync(const char* path) {
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    Hist h;
    for (int i = 0; i < kReports; ++i) {
        LogEntry e = make_report(uint64_t(i));
        uint64_t t0 = monotonic_ns();
        ssize_t n = ::write(fd, &e, sizeof(e));
        uint64_t t1 = monotonic_ns();
        (void)n;
        h.rec(t1 - t0);
    }
    ::fsync(fd);
    ::close(fd);
    return h;
}

// ── Modes 2 & 3: ring + dedicated logger thread ──────────────────────────────
//
// Template on the logger type so the matching-engine-side code (the part
// that matters for the comparison) is IDENTICAL between the classic and
// io_uring runs — no incidental differences to explain away.

template<typename LoggerT, typename LogFn>
Hist run_ring_plus_logger(LoggerT& logger, LogFn log_fn, Hist* completion_hist) {
    static SPSCRingBuffer<LogEntry, 4096> ring;
    std::atomic<bool> producer_done{false};
    Hist push_hist;

    std::thread logger_thread([&] {
        uint64_t seq = 0;
        while (!producer_done.load(std::memory_order_acquire) || !ring.empty()) {
            if (auto e = ring.try_pop()) {
                log_fn(logger, seq++, *e);
            } else {
                __builtin_ia32_pause();
            }
        }
        logger.flush();
    });

    for (int i = 0; i < kReports; ++i) {
        LogEntry e = make_report(uint64_t(i));
        uint64_t t0 = monotonic_ns();
        while (!ring.try_push(e)) __builtin_ia32_pause(); // backpressure if the ring fills
        uint64_t t1 = monotonic_ns();
        push_hist.rec(t1 - t0);
    }
    producer_done.store(true, std::memory_order_release);
    logger_thread.join();
    (void)completion_hist;
    return push_hist;
}

} // namespace

int main(int argc, char** argv) {
    // sqpoll_cpu: pin the kernel's SQ-polling thread to a dedicated core.
    // This project's own development sandbox has 1 CPU, where there is no
    // second core to pin the poll thread to — the "good case" for SQPOLL
    // (a genuinely dedicated poll core) could never be validated there,
    // only the documented failure mode (no dedicated core available).
    // Pass a real core number on multi-core hardware to actually test it:
    // e.g. `./matching_engine_demo 7` pins the SQ poll thread to core 7.
    const int sqpoll_cpu = (argc > 1) ? std::atoi(argv[1]) : -1;
    std::printf("=== Matching Engine -> Logger: End-to-End Comparison ===\n");
    std::printf("%d execution reports per mode, single matching-engine thread.\n\n", kReports);

    // ── Mode 1 ──────────────────────────────────────────────────────────────
    std::printf("--- Mode 1: naive synchronous write() (no ring, no 2nd thread) ---\n");
    Hist naive = run_naive_sync("/tmp/demo_naive.bin");
    print_hist("matching-engine thread", naive);

    // ── Mode 2 ──────────────────────────────────────────────────────────────
    std::printf("\n--- Mode 2: SPSC ring + dedicated thread + blocking write() ---\n");
    ClassicLogger classic;
    if (!classic.open("/tmp/demo_classic.bin")) {
        std::printf("failed to open classic log file\n");
        return 1;
    }
    Hist classic_complete;
    classic.set_on_complete([&](uint64_t, int64_t ns, int32_t) { classic_complete.rec(uint64_t(ns)); });
    int classic_log_failures = 0;
    Hist classic_push = run_ring_plus_logger(classic,
        [&](ClassicLogger& l, uint64_t seq, const LogEntry& e) {
            if (!l.log(seq, e)) ++classic_log_failures;
        },
        &classic_complete);
    classic.close();
    print_hist("matching-engine thread", classic_push);
    print_hist("logger thread (write())", classic_complete);
    std::printf("logger: %lu written, %lu errors, %d log() call failures\n",
                (unsigned long)classic.written_count(), (unsigned long)classic.error_count(),
                classic_log_failures);

    // ── Mode 3 ──────────────────────────────────────────────────────────────
    std::printf("\n--- Mode 3: SPSC ring + dedicated thread + io_uring ---\n");
    IOURingLogger uring_logger;
    if (!uring_logger.open("/tmp/demo_iouring.bin")) {
        std::printf("failed to open io_uring log file\n");
        return 1;
    }
    Hist uring_complete, uring_submit;
    uring_logger.set_on_complete([&](uint64_t, int64_t ns, int32_t) { uring_complete.rec(uint64_t(ns)); });
    int uring_log_failures = 0;
    Hist uring_push = run_ring_plus_logger(uring_logger,
        [&](IOURingLogger& l, uint64_t, const LogEntry& e) {
            uint64_t t0 = monotonic_ns();
            bool ok = l.log(e);
            uint64_t t1 = monotonic_ns();
            uring_submit.rec(t1 - t0);
            if (!ok) ++uring_log_failures;
        },
        &uring_complete);
    uring_logger.close();
    print_hist("matching-engine thread", uring_push);
    print_hist("logger thread (submit)", uring_submit);
    print_hist("logger thread (complete)", uring_complete);
    std::printf("logger: %lu submitted, %lu completed, %lu errors, %d log() call failures\n",
                (unsigned long)uring_logger.submitted_count(),
                (unsigned long)uring_logger.completed_count(),
                (unsigned long)uring_logger.error_count(),
                uring_log_failures);

    // ── Mode 4: SQPOLL — does removing the submission syscall change the
    // completion-latency picture, or only submission cost? Measured in the
    // same harness rather than assumed. ──────────────────────────────────────
    std::printf("\n--- Mode 4: SPSC ring + dedicated thread + io_uring SQPOLL ---\n");
    if (sqpoll_cpu >= 0)
        std::printf("SQ poll thread will be pinned to core %d (pass no argument for unpinned)\n", sqpoll_cpu);
    else
        std::printf("SQ poll thread is unpinned — pass a core number (e.g. `%s 7`) to pin it\n", argv[0]);
    IOURingLoggerConfig sqpoll_cfg;
    sqpoll_cfg.sqpoll = true;
    sqpoll_cfg.sqpoll_idle_ms = 2000;
    sqpoll_cfg.sqpoll_cpu = sqpoll_cpu;
    IOURingLogger sqpoll_logger;
    bool have_sqpoll = sqpoll_logger.open("/tmp/demo_sqpoll.bin", sqpoll_cfg);
    if (!have_sqpoll) {
        std::printf("SQPOLL unavailable in this environment (errno=%d) — "
                    "requires CAP_SYS_NICE. Skipping Mode 4.\n", sqpoll_logger.sqpoll_setup_errno());
    } else {
        Hist sqpoll_complete, sqpoll_submit;
        sqpoll_logger.set_on_complete([&](uint64_t, int64_t ns, int32_t) { sqpoll_complete.rec(ns); });
        int sqpoll_log_failures = 0;
        Hist sqpoll_push = run_ring_plus_logger(sqpoll_logger,
            [&](IOURingLogger& l, uint64_t, const LogEntry& e) {
                uint64_t t0 = monotonic_ns();
                bool ok = l.log(e);
                uint64_t t1 = monotonic_ns();
                sqpoll_submit.rec(int64_t(t1 - t0));
                if (!ok) ++sqpoll_log_failures;
            },
            &sqpoll_complete);
        sqpoll_logger.close();
        print_hist("matching-engine thread", sqpoll_push);
        print_hist("logger thread (submit)", sqpoll_submit);
        print_hist("logger thread (complete)", sqpoll_complete);
        std::printf("logger: %lu submitted, %lu completed, %lu errors, %d log() call failures\n",
                    (unsigned long)sqpoll_logger.submitted_count(),
                    (unsigned long)sqpoll_logger.completed_count(),
                    (unsigned long)sqpoll_logger.error_count(),
                    sqpoll_log_failures);
    }

    // ── Correctness check: every mode must have logged every report ─────────
    auto file_size = [](const char* path) -> long {
        FILE* f = fopen(path, "rb");
        if (!f) return -1;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        return sz;
    };
    long naive_sz   = file_size("/tmp/demo_naive.bin");
    long classic_sz = file_size("/tmp/demo_classic.bin");
    long uring_sz   = file_size("/tmp/demo_iouring.bin");
    const long expected = long(kReports) * 40L;
    long sqpoll_sz  = have_sqpoll ? file_size("/tmp/demo_sqpoll.bin") : expected;

    std::printf("\n--- Correctness (every report must reach disk) ---\n");
    std::printf("naive-sync   : %ld / %ld bytes  %s\n", naive_sz, expected, naive_sz == expected ? "OK" : "MISMATCH");
    std::printf("ring+classic : %ld / %ld bytes  %s\n", classic_sz, expected, classic_sz == expected ? "OK" : "MISMATCH");
    std::printf("ring+iouring : %ld / %ld bytes  %s\n", uring_sz, expected, uring_sz == expected ? "OK" : "MISMATCH");
    if (have_sqpoll)
        std::printf("ring+sqpoll  : %ld / %ld bytes  %s\n", sqpoll_sz, expected, sqpoll_sz == expected ? "OK" : "MISMATCH");
    else
        std::printf("ring+sqpoll  : skipped (SQPOLL unavailable)\n");

    const bool all_ok = (naive_sz == expected) && (classic_sz == expected) && (uring_sz == expected) &&
                        (!have_sqpoll || sqpoll_sz == expected);
    std::printf("\n%s\n", all_ok ? "PASS: all three modes logged every report correctly."
                                  : "FAIL: at least one mode lost or corrupted reports.");
    return all_ok ? 0 : 1;
}
