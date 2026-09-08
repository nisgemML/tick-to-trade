#include "hft/feed.hpp"
#include "hft/pipeline.hpp"
#include <cassert>
#include <cstdio>

// Determinism: two independent pure streams with same seed produce same
// pipeline outcomes when run sequentially with full stop between runs.
int main() {
    hft::SyntheticFeedConfig fc;
    fc.symbol = 1;
    fc.n_events = 3000;
    fc.seed = 42;

    auto run_once = [&](const char* logpath) {
        hft::PipelineConfig cfg;
        cfg.engine_cpu = -1;
        cfg.symbol = 1;
        cfg.log_path = logpath;
        hft::Pipeline pipe(cfg);
        pipe.run_stream(hft::make_synthetic_stream(fc));
        return pipe.result();
    };

    const auto ra = run_once("cons_a.log");
    const auto rb = run_once("cons_b.log");

    std::printf("runA fills=%llu qty=%llu msgs=%llu accepted=%llu\n",
                (unsigned long long)ra.fills,
                (unsigned long long)ra.fill_qty_total,
                (unsigned long long)ra.engine_messages,
                (unsigned long long)ra.events_accepted);
    std::printf("runB fills=%llu qty=%llu msgs=%llu accepted=%llu\n",
                (unsigned long long)rb.fills,
                (unsigned long long)rb.fill_qty_total,
                (unsigned long long)rb.engine_messages,
                (unsigned long long)rb.events_accepted);

    assert(ra.events_submitted == 3000);
    assert(rb.events_submitted == 3000);
    assert(ra.events_accepted == rb.events_accepted);
    assert(ra.fills == rb.fills);
    assert(ra.fill_qty_total == rb.fill_qty_total);
    assert(ra.fills > 0);
    std::printf("test_conservation OK\n");
    return 0;
}
