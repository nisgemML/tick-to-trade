#include "hft/pipeline.hpp"
#include <cassert>
#include <cstdio>
int main() {
    hft::PipelineConfig cfg;
    cfg.engine_cpu = -1;
    cfg.log_path = "test_fills.log";
    cfg.symbol = 1;
    hft::Pipeline pipe(cfg);
    pipe.run_synthetic(2000);
    const auto& r = pipe.result();
    std::printf("smoke: submitted=%llu accepted=%llu fills=%llu rejects=%llu msgs=%llu\n",
                (unsigned long long)r.events_submitted,
                (unsigned long long)r.events_accepted,
                (unsigned long long)r.fills,
                (unsigned long long)r.queue_full_rejects,
                (unsigned long long)r.engine_messages);
    assert(r.events_submitted == 2000);
    assert(r.events_accepted == 2000);
    assert(r.fills > 0);
    std::printf("test_pipeline_smoke OK\n");
    return 0;
}
