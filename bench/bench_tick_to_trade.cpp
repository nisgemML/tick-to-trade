#include "hft/pipeline.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

// Steady-state software tick-to-trade: submit at controlled pace while drain runs.
// Burst mode (argv2=burst) floods the queue to show queueing tail latency.

int main(int argc, char** argv) {
    uint64_t n = 20000;
    bool burst = false;
    if (argc > 1) n = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2 && std::string(argv[2]) == "burst") burst = true;

    hft::PipelineConfig pcfg;
    pcfg.engine_cpu = -1;
    pcfg.log_path = "ttt_fills.log";
    pcfg.symbol = 1;
    hft::Pipeline pipe(pcfg);
    if (!pipe.start()) return 1;

    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t recv = uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        engine::MarketDataMsg m{};
        m.seq = i + 1;
        m.order_id = i + 1;
        m.symbol = 1;
        m.side = (i % 2 == 0) ? engine::Side::Buy : engine::Side::Sell;
        m.msg_type = engine::MarketDataMsg::Type::NewOrder;
        m.order_type = engine::OrderType::Limit;
        m.price = engine::to_price(100.0) + engine::Price(((int)(i % 21) - 10) * 1000);
        m.qty = 10 + engine::Qty(i % 40);
        while (!pipe.submit_with_ts(m, recv)) {
            std::this_thread::yield();
        }
        if (!burst && (i % 32 == 0)) std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    pipe.stop();
    const auto t1 = std::chrono::steady_clock::now();

    const auto& r = pipe.result();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("=== tick-to-trade software path (%s) ===\n", burst ? "burst" : "steady");
    std::printf("events      : %llu\n", (unsigned long long)r.events_accepted);
    std::printf("fills       : %llu\n", (unsigned long long)r.fills);
    std::printf("ttt_samples : %llu\n", (unsigned long long)r.tick_to_trade.count());
    std::printf("ttt_p50_ns  : %llu\n", (unsigned long long)r.tick_to_trade.percentile(0.50));
    std::printf("ttt_p99_ns  : %llu\n", (unsigned long long)r.tick_to_trade.percentile(0.99));
    std::printf("ttt_p999_ns : %llu\n", (unsigned long long)r.tick_to_trade.percentile(0.999));
    std::printf("ttt_max_ns  : %llu\n", (unsigned long long)r.tick_to_trade.max());
    std::printf("throughput  : %.0f msg/s\n", r.events_accepted / sec);
    std::printf("NOTE: unpinned software path; not exchange wire latency.\n");
    return 0;
}
