#pragma once
#include "core/matching_engine.hpp"
#include "core/types.hpp"
#include "hft/feed.hpp"
#include "hft/latency.hpp"
#include "hft/log_sink.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
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
    LatencyHistogram tick_to_trade;
};

class Pipeline {
public:
    explicit Pipeline(PipelineConfig cfg) : cfg_(std::move(cfg)) {}
    ~Pipeline() { stop(); }

    bool start() {
        if (running_.load(std::memory_order_acquire)) return true;
        (void)engine_.register_symbol(cfg_.symbol);
        if (!sink_.open(cfg_.log_path.c_str())) return false;
        engine_.start(cfg_.engine_cpu);
        running_.store(true, std::memory_order_release);
        drain_ = std::thread([this] { drain_loop(); });
        return true;
    }

    void stop() {
        if (!running_.load(std::memory_order_acquire)) return;
        running_.store(false, std::memory_order_release);
        engine_.stop();
        if (drain_.joinable()) drain_.join();
        sink_.close();
        result_.engine_messages = engine_.messages_processed();
        result_.engine_matches  = engine_.matches_generated();
    }

    bool submit(const engine::MarketDataMsg& msg) {
        return submit_with_ts(msg, 0);
    }

    bool submit_with_ts(const engine::MarketDataMsg& msg, uint64_t recv_ns) {
        ++result_.events_submitted;
        if (recv_ns != 0 && msg.msg_type == engine::MarketDataMsg::Type::NewOrder) {
            // recv_by_order_ is written here (producer/main-thread context)
            // and read/erased in on_fill() (drain thread) — a genuine,
            // TSan-confirmed data race existed here: a plain
            // std::unordered_map mutated and read from two threads with
            // zero synchronization, worse than the running_ race fixed
            // earlier because it's a full STL container (including
            // internal rehashing), not a single scalar. Neither of these
            // two call sites is on options-engine's own matching hot path
            // — this is purely tick-to-trade measurement bookkeeping, one
            // insert per submitted order and one lookup per fill — so a
            // mutex is the right tool here, not a lock-free structure.
            std::lock_guard<std::mutex> lock(recv_mu_);
            recv_by_order_[msg.order_id] = recv_ns;
        }
        if (engine_.submit(msg)) {
            ++result_.events_accepted;
            return true;
        }
        ++result_.queue_full_rejects;
        return false;
    }

    // Submit only — no fixed sleep. Callers time this for throughput.
    void submit_stream(const std::vector<engine::MarketDataMsg>& stream) {
        for (const auto& m : stream) {
            while (!submit(m)) std::this_thread::yield();
        }
    }

    // Poll until engine message count catches accepted (or timeout).
    // Replaces fixed 200ms sleep so benches don't time sleep.
    void wait_until_drained(uint64_t expected_msgs, int timeout_ms = 5000) {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
        while (clock::now() < deadline) {
            if (engine_.messages_processed() >= expected_msgs) {
                // brief settle for outbound drain
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return;
            }
            std::this_thread::yield();
        }
    }

    void run_stream(const std::vector<engine::MarketDataMsg>& stream) {
        if (!start()) return;
        submit_stream(stream);
        wait_until_drained(stream.size());
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
        while (running_.load(std::memory_order_acquire)) {
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
        {
            // Same recv_by_order_ map as submit_with_ts() — same lock.
            // Also erase the entry once it's been used: the previous
            // version never erased anything, so this map grew unbounded
            // for the entire lifetime of the Pipeline (every NewOrder
            // that ever carried a recv_ns stayed in it forever, whether
            // or not it had already been matched). Harmless for a
            // short, bounded benchmark run; a genuine, unbounded memory
            // leak for any long-running use — a soak test, or a real
            // deployment — which is exactly the class of thing this
            // portfolio's own components (options-engine, io-uring-queue)
            // have each been through a real bug-finding pass to catch
            // elsewhere. Fixed here at the same time as the race, since
            // both live in the same two lines.
            //
            // Trade-off worth being explicit about: an order filled
            // across several partial ExecutionReports now gets its
            // tick-to-trade sample recorded once, on its FIRST fill, not
            // on every subsequent partial fill of the same order_id (the
            // entry is gone after that). This matches the more common
            // definition of tick-to-trade — time to first reaction, not
            // every increment of it — and is the honest cost of no
            // longer leaking one entry per order for the life of the
            // process.
            std::lock_guard<std::mutex> lock(recv_mu_);
            auto it = recv_by_order_.find(rpt.order_id);
            if (it == recv_by_order_.end())
                it = recv_by_order_.find(rpt.contra_order_id);
            if (it != recv_by_order_.end()) {
                if (t >= it->second) result_.tick_to_trade.record(t - it->second);
                recv_by_order_.erase(it);
            }
        }

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
    std::atomic<bool> running_{false};
    PipelineResult result_{};
    std::mutex recv_mu_;
    std::unordered_map<engine::OrderId, uint64_t> recv_by_order_;
};

} // namespace hft
