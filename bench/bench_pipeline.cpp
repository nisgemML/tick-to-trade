#include "hft/feed.hpp"
#include "hft/pipeline.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>

// Throughput of submit path while drain runs concurrently.
// Does NOT include a fixed sleep in the timed region.

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

    hft::Pipeline pipe(cfg);
    if (!pipe.start()) return 1;

    const auto t0 = std::chrono::steady_clock::now();
    pipe.submit_stream(stream);
    const auto t1 = std::chrono::steady_clock::now();

    pipe.wait_until_drained(n);
    pipe.stop();
    const auto t2 = std::chrono::steady_clock::now();

    const auto submit_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const auto total_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count();
    const auto& r = pipe.result();
    const double submit_sec = submit_us / 1e6;
    const double total_sec = total_us / 1e6;

    std::printf("=== hft-stack pipeline bench ===\n");
    std::printf("events            : %llu\n", (unsigned long long)r.events_accepted);
    std::printf("fills             : %llu\n", (unsigned long long)r.fills);
    std::printf("submit_only_us    : %lld\n", (long long)submit_us);
    std::printf("submit+drain_us   : %lld\n", (long long)total_us);
    std::printf("throughput_submit : %.2f msg/s\n",
                submit_sec > 0 ? r.events_accepted / submit_sec : 0.0);
    std::printf("throughput_e2e    : %.2f msg/s\n",
                total_sec > 0 ? r.events_accepted / total_sec : 0.0);
    std::printf("queue_full        : %llu\n", (unsigned long long)r.queue_full_rejects);
    std::printf("NOTE: unpinned software path — not co-location wire latency.\n");
    std::printf("NOTE: submit_only excludes drain wait; e2e includes poll-until-drained.\n");
    return 0;
}
