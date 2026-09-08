// bench/bench_ring.cpp — SPSCRingBuffer throughput + io_uring write latency benchmark.
//
// Measures:
//   1. SPSC ring buffer: single-thread push+pop throughput and latency
//   2. io_uring vs synchronous write: async log submission latency

#include "ioq/ring.hpp"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <x86intrin.h>
#include <fcntl.h>
#include <unistd.h>

static double g_ns_per_cycle = 1.0;
static void calibrate() {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = __rdtsc();
    uint64_t x = 0;
    for (int i = 0; i < 100000000; ++i) x += i;
    uint64_t c1 = __rdtsc();
    auto t1 = std::chrono::steady_clock::now();
    g_ns_per_cycle = double(std::chrono::duration_cast<
        std::chrono::nanoseconds>(t1-t0).count()) / double(c1-c0);
    (void)x;
}

// Raw-sample histogram — a fixed-range bucketed histogram (this file's
// previous version capped at N=4096ns and silently folded anything larger
// into the last bucket) produces exactly-equal p99/p99.9 values whenever
// the real tail exceeds the cap — which is precisely what this file's own
// previous io_uring benchmark output showed (p99=4095, p99.9=4095,
// identically), and it's a silent-failure signature, not a coincidence.
// Storing raw samples and sorting avoids the question of what range to
// pick entirely; 2M int64_t samples (~16MB) is a non-issue at this scale.
#include <algorithm>
#include <vector>
struct Hist {
    std::vector<int64_t> v;
    void rec(int64_t x) { v.push_back(x); }
    void finalize() { std::sort(v.begin(), v.end()); }
    uint64_t total() const { return v.size(); }
    int64_t pct(double p) const {
        if (v.empty()) return 0;
        size_t idx = size_t(p * double(v.size() - 1));
        return v[idx];
    }
};

int main() {
    calibrate();
    printf("=== SPSC Ring Buffer + io_uring Benchmark ===\n");
    printf("TSC: %.3f ns/cycle\n\n", g_ns_per_cycle);

    constexpr int N = 2000000;
    constexpr int WARMUP = 100000;

    // ── 1. SPSC single-thread push+pop latency ────────────────────────────────
    printf("--- SPSC Ring Buffer (single-thread push+pop) ---\n");
    ioq::SPSCRingBuffer<uint64_t, 1024> ring;
    Hist h_push, h_pop;
    uint64_t sink = 0;  // not volatile — C++20 deprecated volatile compound assignment

    for (int i = 0; i < WARMUP; ++i) {
        (void)ring.try_push(uint64_t(i));
        if (auto v = ring.try_pop()) sink += *v;
    }

    for (int i = 0; i < N; ++i) {
        uint64_t t0 = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
        (void)ring.try_push(uint64_t(i));
        uint64_t t1 = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
        h_push.rec(t1 - t0);

        t0 = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
        if (auto v = ring.try_pop()) sink += *v;
        t1 = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
        h_pop.rec(t1 - t0);
    }

    h_push.finalize();
    h_pop.finalize();
    printf("push latency: p50=%lldns p99=%lldns p99.9=%lldns\n",
        (long long)h_push.pct(0.50),
        (long long)h_push.pct(0.99),
        (long long)h_push.pct(0.999));
    printf("pop  latency: p50=%lldns p99=%lldns p99.9=%lldns\n",
        (long long)h_pop.pct(0.50),
        (long long)h_pop.pct(0.99),
        (long long)h_pop.pct(0.999));

    // ── 2. io_uring submit latency ────────────────────────────────────────────
    printf("\n--- io_uring async write submit latency ---\n");
    ioq::IOURingLogger logger;
    if (!logger.open("/tmp/bench_iouring.bin")) {
        printf("io_uring unavailable in this environment\n");
        printf("(requires Linux 5.1+, kernel.io_uring_disabled=0)\n");
        goto done;
    }
    {
        Hist h_iouring;
        constexpr int M = 100000;

        ioq::LogEntry e{};
        e.order_id = 1; e.price = 10000; e.qty = 100;
        e.side = 'B'; memcpy(e.symbol, "AAPL  ", 6); e.event_type = 'F';

        // Warmup
        for (int i = 0; i < 1000; ++i) {
            e.timestamp_ns = uint64_t(i);
            (void)logger.log(e);
        }
        logger.flush();

        // Timed
        for (int i = 0; i < M; ++i) {
            e.timestamp_ns = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
            uint64_t t0 = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
            (void)logger.log(e);
            uint64_t t1 = uint64_t(double(__rdtsc()) * g_ns_per_cycle);
            h_iouring.rec(t1 - t0);
        }
        logger.flush();
        logger.close();

        h_iouring.finalize();
        printf("io_uring submit: p50=%lldns p99=%lldns p99.9=%lldns\n",
            (long long)h_iouring.pct(0.50),
            (long long)h_iouring.pct(0.99),
            (long long)h_iouring.pct(0.999));
        printf("\nCompare vs synchronous write():\n");
        printf("  write() typical: 500–5,000 ns (syscall + kernel scheduling)\n");
        printf("  io_uring submit: %lld ns p50 (SQ store + io_uring_enter)\n",
            (long long)h_iouring.pct(0.50));
        printf("  io_uring SQPOLL: see docs/failure-modes-and-batching.md — measured\n");
        printf("  in this environment, NOT the ~5-15ns design estimate (this machine\n");
        printf("  has 1 CPU core; SQPOLL's poll thread has no dedicated core to run on,\n");
        printf("  and completion latency was measured ~48x WORSE than plain io_uring).\n");
    }

done:
    printf("\n(sink=%llu)\n", (unsigned long long)sink);
    return 0;
}
