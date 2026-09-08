#include "hft/check.hpp"
#include "hft/feed.hpp"
#include "hft/pipeline.hpp"
#include <cstdio>

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

    CHECK(ra.events_submitted == 3000, "runA submitted");
    CHECK(rb.events_submitted == 3000, "runB submitted");
    CHECK(ra.events_accepted == rb.events_accepted, "accepted match");
    CHECK(ra.fills == rb.fills, "fills match");
    CHECK(ra.fill_qty_total == rb.fill_qty_total, "fill qty match");
    CHECK(ra.engine_messages == rb.engine_messages, "engine messages match");
    CHECK(ra.fills > 0, "non-zero fills");

    TEST_EXIT();
}
