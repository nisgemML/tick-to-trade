// tools/replay_itch50.cpp
//
// Replays a REAL NASDAQ TotalView-ITCH 5.0 file (the free daily samples published at
// ftp://emi.nasdaq.com/ITCH/, framed as [2-byte big-endian length][message]) for one
// symbol through the tick-to-trade pipeline and matching engine, and checks the
// engine's resulting book against an independent reference book built straight from
// the same messages.
//
//   gzip -dc 01302020.NASDAQ_ITCH50.gz | ./replay_itch50 - --symbol AAPL --json
//   ./replay_itch50 file.NASDAQ_ITCH50 --symbol AAPL --engine-start-ns 34230000000000
//
// Options:
//   --symbol S             required; matched against the 'R' Stock Directory messages
//   --max-msgs N           stop after N messages read (0 = whole file)
//   --no-engine            reference book only (fast pass over a multi-GB file)
//   --engine-start-ns T    feed the engine from a snapshot at timestamp T (ns since
//                          midnight) instead of from the start of day. Real days have a
//                          pre-open book that is legitimately crossed until the opening
//                          cross runs; 34230000000000 = 09:30:30 ET starts the engine
//                          after the open, where the displayed book should not cross.
//   --assume-engine-capacity N   test hook: treat the engine's resting-order capacity as N
//   --log PATH             fill log path (default itch50_fills.log)
//   --json                 print one JSON summary line on stdout
//
// Exit code: 0 = completed and (if the engine ran) its book matches the reference;
//            1 = completed but the engine's book disagrees; 2 = usage/IO; 3 = symbol not found;
//            4 = the symbol's book exceeded the engine's fixed capacity (OrderBook::kMaxOrders =
//                65,536 live orders -- documented in options-engine's LIMITATIONS.md), so the
//                engine's book is truncated and comparing it to the reference is not meaningful.

#include "hft/itch50.hpp"
#include "hft/pipeline.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace hft;

namespace {
struct Args {
    std::string path, symbol, log{"itch50_fills.log"};
    uint64_t max_msgs{0}, engine_start_ns{0}, assume_capacity{0};
    bool no_engine{false}, json{false};
};

bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        if (s == "--symbol")               { const char* v = need("--symbol"); if (!v) return false; a.symbol = v; }
        else if (s == "--max-msgs")        { const char* v = need("--max-msgs"); if (!v) return false; a.max_msgs = std::strtoull(v, nullptr, 10); }
        else if (s == "--engine-start-ns") { const char* v = need("--engine-start-ns"); if (!v) return false; a.engine_start_ns = std::strtoull(v, nullptr, 10); }
        else if (s == "--assume-engine-capacity") { const char* v = need("--assume-engine-capacity"); if (!v) return false; a.assume_capacity = std::strtoull(v, nullptr, 10); }
        else if (s == "--log")             { const char* v = need("--log"); if (!v) return false; a.log = v; }
        else if (s == "--no-engine")       a.no_engine = true;
        else if (s == "--json")            a.json = true;
        else if (!s.empty() && s[0] == '-' && s != "-") { std::fprintf(stderr, "unknown option %s\n", s.c_str()); return false; }
        else a.path = s;
    }
    return !a.path.empty() && !a.symbol.empty();
}
} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse_args(argc, argv, a)) {
        std::fprintf(stderr, "usage: %s <file|-> --symbol SYM [--max-msgs N] [--no-engine] [--engine-start-ns T] [--log PATH] [--json]\n", argv[0]);
        return 2;
    }
    FILE* f = (a.path == "-") ? stdin : std::fopen(a.path.c_str(), "rb");
    if (!f) { std::perror("open"); return 2; }
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);

    PipelineConfig pcfg; pcfg.log_path = a.log;
    Pipeline pipeline(pcfg);
    if (!a.no_engine && !pipeline.start()) { std::fprintf(stderr, "pipeline start failed\n"); return 2; }

    itch50::RefBook ref;
    itch50::Translator tr(pcfg.symbol);
    int64_t target = -1;
    uint64_t counts[256]{}, sym_counts[256]{};
    uint64_t messages_read = 0, malformed = 0, truncated = 0, sent = 0, retries = 0, crossed_events = 0;
    bool engine_started = (a.engine_start_ns == 0);
    const uint64_t capacity = a.assume_capacity ? a.assume_capacity : engine::OrderBook::kMaxOrders;
    uint64_t peak_resting = 0, first_exceeded_at = 0;
    std::vector<uint8_t> buf(65536);

    auto send = [&](const engine::MarketDataMsg& m) {
        if (a.no_engine) return;
        uint64_t spins = 0;
        while (!pipeline.submit(m)) {
            ++retries; std::this_thread::yield();
            if (++spins > 50'000'000ULL) { std::fprintf(stderr, "engine queue stuck, aborting\n"); std::exit(2); }
        }
        ++sent;
    };

    for (;;) {
        if (a.max_msgs && messages_read >= a.max_msgs) break;
        uint8_t lb[2];
        const std::size_t r = std::fread(lb, 1, 2, f);
        if (r == 0) break;
        if (r < 2) { ++truncated; break; }
        const uint16_t len = itch50::be16(lb);
        if (len == 0) { ++malformed; continue; }
        if (std::fread(buf.data(), 1, len, f) != len) { ++truncated; break; }
        ++messages_read; ++counts[buf[0]];

        itch50::Header h;
        if (!itch50::parse_header(buf.data(), len, h)) { ++malformed; continue; }

        if (h.type == 'R') {
            itch50::StockDirectory d;
            if (itch50::StockDirectory::parse(buf.data(), len, d) && itch50::trim_symbol(d.stock) == a.symbol) target = h.locate;
            continue;
        }
        if (target < 0 || h.locate != target) continue;

        const bool book_msg = (h.type == 'A' || h.type == 'F' || h.type == 'E' || h.type == 'C' ||
                               h.type == 'X' || h.type == 'D' || h.type == 'U');
        if (!book_msg) continue;

        if (!engine_started && h.timestamp_ns >= a.engine_start_ns) {
            for (const auto& m : tr.snapshot()) send(m);
            engine_started = true;
        }

        itch50::Translator::Out out;
        bool ok = true;
        switch (h.type) {
        case 'A': case 'F': { itch50::AddOrder m; if ((ok = itch50::AddOrder::parse(buf.data(), len, m))) { ref.add(m.order_ref, m.side, m.price, m.shares); out = tr.on_add(m.order_ref, m.side, m.price, m.shares); } break; }
        case 'E': case 'C': { itch50::OrderExecuted m; if ((ok = itch50::OrderExecuted::parse(buf.data(), len, m))) { ref.reduce(m.order_ref, m.shares); out = tr.on_reduce(m.order_ref, m.shares, true); } break; }
        case 'X': { itch50::OrderCancel m; if ((ok = itch50::OrderCancel::parse(buf.data(), len, m))) { ref.reduce(m.order_ref, m.canceled_shares); out = tr.on_reduce(m.order_ref, m.canceled_shares, false); } break; }
        case 'D': { itch50::OrderDelete m; if ((ok = itch50::OrderDelete::parse(buf.data(), len, m))) { ref.remove(m.order_ref); out = tr.on_delete(m.order_ref); } break; }
        case 'U': { itch50::OrderReplace m; if ((ok = itch50::OrderReplace::parse(buf.data(), len, m))) { ref.replace(m.orig_ref, m.new_ref, m.shares, m.price); out = tr.on_replace(m.orig_ref, m.new_ref, m.shares, m.price); } break; }
        default: break;
        }
        if (!ok) { ++malformed; continue; }
        ++sym_counts[h.type];
        if (ref.crossed()) ++crossed_events;
        if (engine_started && !a.no_engine) {
            peak_resting = std::max<uint64_t>(peak_resting, tr.resting());
            if (!first_exceeded_at && tr.resting() > capacity) first_exceeded_at = messages_read;
        }
        if (engine_started) for (int i = 0; i < out.n; ++i) send(out.msgs[i]);
    }
    if (f != stdin) std::fclose(f);

    if (target >= 0 && !engine_started) { for (const auto& m : tr.snapshot()) send(m); engine_started = true; }
    if (!a.no_engine) { pipeline.wait_until_drained(sent, 600000); pipeline.stop(); }

    const itch50::Bbo rb = ref.bbo();
    engine::BestQuote q{}; bool have_engine = false;
    if (!a.no_engine) {
        if (const engine::OrderBook* bk = pipeline.engine().book_for(pcfg.symbol)) { q = bk->best_quote(); have_engine = true; }
    }
    auto px = [](engine::Price p) -> uint64_t { return p == engine::PRICE_INVALID ? 0 : static_cast<uint64_t>(p / 100); };
    const PipelineResult& res = pipeline.result();
    const bool cap_exceeded = first_exceeded_at != 0;
    const bool agree = !have_engine || (!cap_exceeded &&
        (px(q.bid_price) == rb.bid_px && q.bid_qty == rb.bid_qty && px(q.ask_price) == rb.ask_px && q.ask_qty == rb.ask_qty &&
         res.engine_messages == sent));
    const auto& tc = tr.counters(); const auto& an = ref.anomalies();
    auto U = [](uint64_t v) { return static_cast<unsigned long long>(v); };

    std::fprintf(stderr,
        "symbol=%s found=%d locate=%lld | read=%llu malformed=%llu truncated=%llu | symbol book msgs: A=%llu F=%llu E=%llu C=%llu X=%llu D=%llu U=%llu\n"
        "ref book: orders=%zu bbo=%u x %llu / %u x %llu levels=%zu/%zu crossed_events=%llu anomalies(unknown=%llu over_reduce=%llu dup=%llu)\n",
        a.symbol.c_str(), target >= 0, (long long)target, U(messages_read), U(malformed), U(truncated),
        U(sym_counts['A']), U(sym_counts['F']), U(sym_counts['E']), U(sym_counts['C']), U(sym_counts['X']), U(sym_counts['D']), U(sym_counts['U']),
        rb.orders, rb.bid_px, U(rb.bid_qty), rb.ask_px, U(rb.ask_qty), rb.bid_levels, rb.ask_levels, U(crossed_events), U(an.unknown_ref), U(an.over_reduce), U(an.duplicate_ref));
    if (cap_exceeded)
        std::fprintf(stderr,
            "ENGINE CAPACITY EXCEEDED: peak %llu resting orders > engine capacity %llu (first at message %llu). The engine cannot rest the excess\n"
            "orders (fixed-size slot pool, documented in options-engine LIMITATIONS.md), so its book is truncated and the comparison below is NOT\n"
            "meaningful. Re-run with --no-engine for the reference book, or choose a less active symbol.\n",
            U(peak_resting), U(capacity), U(first_exceeded_at));
    if (have_engine)
        std::fprintf(stderr, "engine:   sent=%llu processed=%llu fills=%llu matches=%llu bbo=%llu x %u / %llu x %u retries=%llu | engine book %s reference\n",
            U(sent), U(res.engine_messages), U(res.fills), U(res.engine_matches), U(px(q.bid_price)), q.bid_qty, U(px(q.ask_price)), q.ask_qty, U(retries), agree ? "MATCHES" : "DISAGREES WITH");

    if (a.json) {
        std::printf("{\"symbol\":\"%s\",\"found\":%s,\"target_locate\":%lld,\"messages_read\":%llu,\"malformed\":%llu,\"truncated\":%llu,"
                    "\"per_type\":{", a.symbol.c_str(), target >= 0 ? "true" : "false", (long long)target, U(messages_read), U(malformed), U(truncated));
        bool first = true;
        for (const char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U'}) { if (!sym_counts[(uint8_t)t]) continue; std::printf("%s\"%c\":%llu", first ? "" : ",", t, U(sym_counts[(uint8_t)t])); first = false; }
        std::printf("},\"ref\":{\"orders\":%zu,\"bid_px\":%u,\"bid_qty\":%llu,\"ask_px\":%u,\"ask_qty\":%llu,\"bid_levels\":%zu,\"ask_levels\":%zu,\"crossed_events\":%llu,"
                    "\"unknown_ref\":%llu,\"over_reduce\":%llu,\"duplicate_ref\":%llu},",
                    rb.orders, rb.bid_px, U(rb.bid_qty), rb.ask_px, U(rb.ask_qty), rb.bid_levels, rb.ask_levels, U(crossed_events), U(an.unknown_ref), U(an.over_reduce), U(an.duplicate_ref));
        std::printf("\"translator\":{\"adds\":%llu,\"execs\":%llu,\"partial_cancels\":%llu,\"deletes\":%llu,\"replaces\":%llu,\"unknown_ref\":%llu},",
                    U(tc.adds), U(tc.execs), U(tc.partial_cancels), U(tc.deletes), U(tc.replaces), U(tc.unknown_ref));
        std::printf("\"engine\":{\"ran\":%s,\"sent\":%llu,\"processed\":%llu,\"fills\":%llu,\"bid_px\":%llu,\"bid_qty\":%u,\"ask_px\":%llu,\"ask_qty\":%u,\"capacity\":%llu,\"peak_resting\":%llu,\"capacity_exceeded\":%s},\"agree\":%s}\n",
                    have_engine ? "true" : "false", U(sent), U(res.engine_messages), U(res.fills), U(px(q.bid_price)), q.bid_qty, U(px(q.ask_price)), q.ask_qty,
                    U(capacity), U(peak_resting), cap_exceeded ? "true" : "false", agree ? "true" : "false");
    }
    if (target < 0) { std::fprintf(stderr, "symbol '%s' not found in any Stock Directory ('R') message\n", a.symbol.c_str()); return 3; }
    if (cap_exceeded) return 4;
    return agree ? 0 : 1;
}
