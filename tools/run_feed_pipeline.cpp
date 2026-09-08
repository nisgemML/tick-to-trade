#include "hft/itch_adapter.hpp"
#include "hft/pipeline.hpp"
#include "feed/gap_buffer.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

int main(int argc, char** argv) {
    uint64_t n = 5000;
    if (argc > 1) n = std::strtoull(argv[1], nullptr, 10);

    hft::PipelineConfig pcfg;
    pcfg.engine_cpu = -1;
    pcfg.log_path = "feed_fills.log";
    pcfg.symbol = 1;
    hft::Pipeline pipe(pcfg);
    if (!pipe.start()) return 1;

    auto gb = std::make_unique<feed::GapBuffer>();
    hft::ItchAdapter adapter({1}, [&](const engine::MarketDataMsg& m, uint64_t recv_ns) {
        while (!pipe.submit_with_ts(m, recv_ns)) {
            std::this_thread::yield();
        }
    });
    adapter.attach(*gb);

    for (uint64_t i = 0; i < n; ++i) {
        const char side = (i % 2 == 0) ? 'B' : 'S';
        const uint32_t px = 1000000 + static_cast<uint32_t>(((i % 21) - 10) * 100);
        auto pkt = hft::make_mold_add_packet(i + 1, i + 1, side, 10 + (i % 20), px);
        const uint64_t recv = uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        (void)gb->ingest(pkt.data(), pkt.size(), recv);
    }

    pipe.wait_until_drained(n);
    pipe.stop();

    const auto& r = pipe.result();
    std::printf("feed_pipeline submitted=%llu fills=%llu ttt_p50=%llu ttt_p99=%llu samples=%llu\n",
                (unsigned long long)r.events_submitted,
                (unsigned long long)r.fills,
                (unsigned long long)r.tick_to_trade.percentile(0.50),
                (unsigned long long)r.tick_to_trade.percentile(0.99),
                (unsigned long long)r.tick_to_trade.count());
    return 0;
}
