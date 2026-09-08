// bench/bench_batching.cpp — measures the actual effect of
// IOURingLoggerConfig::batch_size on submission cost and throughput,
// rather than asserting batching helps without a number behind it.
//
// Methodology: log() the same number of entries at several batch sizes,
// each into a fresh file, saturating as fast as possible (no artificial
// pacing) — this isolates the LOGGER's own throughput ceiling, which is
// what batch_size actually trades against (fewer io_uring_submit() calls,
// each covering more queued SQEs, at the cost of a queued-but-not-yet-
// submitted entry waiting longer before the kernel even starts on it).

#include "ioq/ring.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ioq;

namespace {
uint64_t monotonic_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
}
}

int main() {
    constexpr int kEntries = 100'000;
    std::vector<unsigned> batch_sizes = {1, 4, 16, 64, 256};

    std::printf("=== Batching Effect on IOURingLogger ===\n");
    std::printf("%d entries per run, saturating (no pacing).\n\n", kEntries);
    std::printf("%-12s %14s %14s %10s\n", "batch_size", "total_ns", "ns/entry", "entries/ms");

    for (unsigned bs : batch_sizes) {
        char path[64];
        std::snprintf(path, sizeof(path), "/tmp/bench_batching_%u.bin", bs);

        IOURingLoggerConfig cfg;
        cfg.batch_size = bs;
        IOURingLogger logger;
        if (!logger.open(path, cfg)) {
            std::printf("%-12u FAILED TO OPEN\n", bs);
            continue;
        }

        LogEntry e{};
        e.order_id = 1; e.price = 100; e.qty = 10;
        e.side = 'B'; e.event_type = 'F';
        std::memcpy(e.symbol, "AAPL  ", 6);

        const uint64_t t0 = monotonic_ns();
        for (int i = 0; i < kEntries; ++i) {
            e.timestamp_ns = uint64_t(i);
            if (!logger.log(e)) { std::printf("log() failed at i=%d, batch_size=%u\n", i, bs); break; }
        }
        logger.flush();
        const uint64_t t1 = monotonic_ns();
        logger.close();

        const double total_ns = double(t1 - t0);
        const double ns_per_entry = total_ns / double(kEntries);
        const double entries_per_ms = double(kEntries) / (total_ns / 1'000'000.0);

        std::printf("%-12u %14.0f %14.1f %10.1f\n", bs, total_ns, ns_per_entry, entries_per_ms);
    }

    std::printf("\nNote: this measures END-TO-END logger throughput (log() calls\n");
    std::printf("through flush() completing), not pure submission-call cost in\n");
    std::printf("isolation — that's the number that actually matters for whether\n");
    std::printf("batching is worth using in a given deployment.\n");
    return 0;
}
