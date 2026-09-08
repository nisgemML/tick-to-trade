#pragma once
#include "core/matching_engine.hpp"
#include "core/types.hpp"
#include "hft/feed.hpp"
#include "hft/latency.hpp"
#include "hft/log_sink.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hft {

struct PipelineConfig {
    int              engine_cpu = -1;
    std::string      log_path   = "fills.log";
    engine::SymbolId symbol     = 1;
};

struct PipelineResult {
    uint64_t events_submitted{0};
    uint64_t events_accepted{0};
    uint64_t fills{0};
    uint64_t queue_full_rejects{0};
    uint64_t engine_messages{0};
    uint64_t engine_matches{0};
    uint64_t fill_qty_total{0};
    LatencyHistogram tick_to_trade; // recv_ns -> fill observe ns (software path)
};

class Pipeline {
public:
    explicit Pipeline(PipelineConfig cfg) : cfg_(std::move(cfg)) {}
    ~Pipeline() { stop(); }

    bool start() {
        if (running_) return true;
        (void)engine_.register_symbol(cfg_.symbol);
        if (!sink_.open(cfg_.log_path.c_str())) return false;
        engine_.start(cfg_.engine_cpu);
        running_ = true;
        drain_ = std::thread([this] { drain_loop(); });
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        engine_.stop();
        if (drain_.joinable()) drain_.join();
        sink_.close();
        result_.engine_messages = engine_.messages_processed();
        result_.engine_matches  = engine_.matches_generated();
    }

    bool submit(const engine::MarketDataMsg& msg) {
        return submit_with_ts(msg, 0);
    }

    // recv_ns from feed SO_TIMESTAMPING / GapBuffer MoldMessage.recv_ns
    bool submit_with_ts(const engine::MarketDataMsg& msg, uint64_t recv_ns) {
        ++result_.events_submitted;
        if (recv_ns != 0 && msg.msg_type == engine::MarketDataMsg::Type::NewOrder) {
            recv_by_order_[msg.order_id] = recv_ns;
        }
        if (engine_.submit(msg)) {
            ++result_.events_accepted;
            return true;
        }
        ++result_.queue_full_rejects;
        return false;
    }

    void run_stream(const std::vector<engine::MarketDataMsg>& stream) {
        if (!start()) return;
        for (const auto& m : stream) {
            while (!submit(m)) std::this_thread::yield();
        }
        for (int i = 0; i < 40; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stop();
    }

    void run_synthetic(uint64_t n) {
        SyntheticFeedConfig fc;
        fc.symbol = cfg_.symbol;
        fc.n_events = n;
        run_stream(make_synthetic_stream(fc));
    }

    const PipelineResult& result() const { return result_; }
    engine::MatchingEngine& engine() { return engine_; }

private:
    static uint64_t now_ns() {
        using namespace std::chrono;
        return uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    }

    void drain_loop() {
        engine::ExecutionReport rpt{};
        while (running_) {
            if (!engine_.poll_report(rpt)) {
                std::this_thread::yield();
                continue;
            }
            on_fill(rpt);
        }
        while (engine_.poll_report(rpt)) on_fill(rpt);
    }

    void on_fill(const engine::ExecutionReport& rpt) {
        ++result_.fills;
        result_.fill_qty_total += rpt.exec_qty;
        const uint64_t t = now_ns();
        auto it = recv_by_order_.find(rpt.order_id);
        if (it == recv_by_order_.end())
            it = recv_by_order_.find(rpt.contra_order_id);
        if (it != recv_by_order_.end() && t >= it->second)
            result_.tick_to_trade.record(t - it->second);

        LogEntry e{};
        e.timestamp_ns = t;
        e.order_id = rpt.order_id;
        e.price = rpt.exec_price;
        e.qty = rpt.exec_qty;
        e.side = (rpt.side == engine::Side::Buy) ? 'B' : 'S';
        e.event_type = 'F';
        e.symbol[0] = 'X';
        (void)sink_.log(e);
    }

    PipelineConfig cfg_;
    engine::MatchingEngine engine_;
    FileLogSink sink_;
    std::thread drain_;
    bool running_{false};
    PipelineResult result_{};
    std::unordered_map<engine::OrderId, uint64_t> recv_by_order_;
};

} // namespace hft
