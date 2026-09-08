#include "hft/feed.hpp"
#include "hft/pipeline.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

// Software pipeline throughput bench.
// For isolated-core claims: taskset -c N chrt -f 50 ./bench_pipeline ...

int main(int argc, char** argv) {
    uint64_t n = 100000;
    if (argc > 1) n = std::strtoull(argv[1], nullptr, 10);

    hft::SyntheticFeedConfig fc;
    fc.n_events = n;
    fc.symbol = 1;
    const auto stream = hft::make_synthetic_stream(fc);

    hft::PipelineConfig cfg;
    cfg.engine_cpu = -1;
    cfg.log_path = "bench_fills.log";
    cfg.symbol = 1;

    const auto t0 = std::chrono::steady_clock::now();
    hft::Pipeline pipe(cfg);
    pipe.run_stream(stream);
    const auto t1 = std::chrono::steady_clock::now();

    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const auto& r = pipe.result();
    const double sec = us / 1e6;
    const double mps = sec > 0 ? r.events_accepted / sec : 0;

    std::printf("=== hft-stack pipeline bench ===\n");
    std::printf("events     : %llu\n", (unsigned long long)r.events_accepted);
    std::printf("fills      : %llu\n", (unsigned long long)r.fills);
    std::printf("wall_us    : %lld\n", (long long)us);
    std::printf("throughput : %.2f msg/s\n", mps);
    std::printf("queue_full : %llu\n", (unsigned long long)r.queue_full_rejects);
    std::printf("NOTE: unpinned software path — not co-location wire latency.\n");
    return 0;
}
