#include "hft/pipeline.hpp"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    uint64_t n = 20000;
    if (argc > 1) n = std::strtoull(argv[1], nullptr, 10);
    hft::PipelineConfig cfg;
    cfg.engine_cpu = -1;
    cfg.log_path = "fills.log";
    cfg.symbol = 1;
    hft::Pipeline pipe(cfg);
    std::printf("hft-stack pipeline: %llu synthetic events\n", (unsigned long long)n);
    pipe.run_synthetic(n);
    const auto& r = pipe.result();
    std::printf("submitted        : %llu\n", (unsigned long long)r.events_submitted);
    std::printf("accepted         : %llu\n", (unsigned long long)r.events_accepted);
    std::printf("queue_full       : %llu\n", (unsigned long long)r.queue_full_rejects);
    std::printf("fills            : %llu\n", (unsigned long long)r.fills);
    std::printf("fill_qty_total   : %llu\n", (unsigned long long)r.fill_qty_total);
    std::printf("engine_messages  : %llu\n", (unsigned long long)r.engine_messages);
    std::printf("engine_matches   : %llu\n", (unsigned long long)r.engine_matches);
    return 0;
}
