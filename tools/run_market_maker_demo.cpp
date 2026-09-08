// tools/run_market_maker_demo.cpp — runs hft::MarketMaker against a real
// MatchingEngine and a synthetic noise-trader order flow, end to end.
//
// This exercises the market maker against real ExecutionReports from
// options-engine's actual matching logic, not a mocked/simulated fill
// stream — the same correctness discipline as everything else in this
// repo. See include/hft/market_maker.hpp's header comment for exactly
// what this does and does not claim: this demonstrates correct
// inventory/P&L mechanics against a synthetic, uncalibrated stream, not
// a profitable trading strategy.

#include "core/matching_engine.hpp"
#include "hft/feed.hpp"
#include "hft/market_maker.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>

using namespace engine;

int main(int argc, char** argv) {
    const uint64_t n_events = (argc > 1) ? uint64_t(std::atoll(argv[1])) : 20000;
    const int64_t position_limit = (argc > 2) ? std::atoll(argv[2]) : 500;
    const bool verbose = (argc > 3) && (std::string(argv[3]) == "verbose");
    constexpr SymbolId kSymbol = 1;
    constexpr int kRequoteEvery = 20;  // refresh our own quote every N noise events

    MatchingEngine mkt_engine;
    mkt_engine.register_symbol(kSymbol);
    mkt_engine.start(-1);  // unpinned — see docs/design.md sec 5(d); this
                            // demo runs one producer thread (this one)
                            // and the engine's own internal thread, no
                            // reason to fight for a dedicated core here.

    hft::MarketMakerConfig cfg;
    // half_spread tuned to this demo's specific synthetic feed, not left
    // at MarketMakerConfig's own default: hft::make_synthetic_stream
    // generates noise-trader prices within +/-10 ticks of mid, where each
    // tick is $0.001 (see feed.hpp) — a +/-$0.01 band. The default
    // half_spread of $0.02 would place both our bid and ask OUTSIDE that
    // entire band, meaning we'd never be competitive enough to get hit —
    // confirmed directly: a first run with the untouched default posted
    // zero fills across 20,000 events, not because anything was broken,
    // but because the quote genuinely never overlapped the noise
    // traders' range. $0.004 sits inside that band with room either
    // side of the spread.
    cfg.half_spread = engine::to_price(0.004);
    cfg.position_limit = position_limit;
    // defaults for everything else: see market_maker.hpp
    hft::MarketMaker mm(kSymbol, cfg, /*account_id=*/999);
    // Note: options-engine's self-trade prevention (Order::account_id)
    // is not actually reachable through MarketDataMsg / submit() — the
    // wire-level message this integration layer submits through has no
    // account_id field at all, only the lower-level Order struct used
    // internally by options-engine's own test suite does. Not needed
    // here regardless: this market maker's own bid is always
    // constructed strictly below its own ask (reservation ± half_spread,
    // half_spread > 0), so its two resting orders can never cross each
    // other — the only way either fills is a genuine counterparty
    // (the noise trader) crossing into it.

    hft::SyntheticFeedConfig fc;
    fc.symbol = kSymbol;
    fc.n_events = n_events;
    auto noise = hft::make_synthetic_stream(fc);

    Price last_trade = fc.mid;
    OrderId next_mm_id = 1'000'000'000;  // disjoint from noise trader's 1..n_events
    OrderId current_bid_id = 0, current_ask_id = 0;
    uint64_t submitted_total = 0;  // every accepted submit, noise + cancel + new-order

    auto submit_retry = [&](const MarketDataMsg& m) {
        while (!mkt_engine.submit(m)) std::this_thread::yield();
        ++submitted_total;
    };

    auto drain_reports = [&]() {
        ExecutionReport rpt{};
        while (mkt_engine.poll_report(rpt)) {
            last_trade = rpt.exec_price;
            mm.on_fill(rpt);
        }
    };

    // Wait until the engine has genuinely processed everything submitted
    // so far, not just enqueued it — mirrors hft::Pipeline::
    // wait_until_drained(). Without this, on this sandbox's constrained
    // core count the engine's own thread can fall arbitrarily far behind
    // a tight submission loop: a first version of this demo submitted
    // all 20,000 events essentially as fast as possible, checked
    // position periodically (which looked flat and well within limits
    // the whole time), then only found out — during a fixed 2-second
    // drain AFTER the loop, when compute_quote() was never called again
    // — that a huge backlog of already-posted orders matched all at
    // once, blowing position far past the configured limit with zero
    // opportunity for the quote-time limit check to ever see it happen.
    // That's a real, worth-knowing lesson about quote-time-only risk
    // checks (they don't protect you from orders that already rest in
    // the book), but it isn't the lesson THIS demo is meant to
    // illustrate, so the engine is now kept caught up throughout.
    auto wait_caught_up = [&](int timeout_ms = 5000) {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
        while (clock::now() < deadline) {
            drain_reports();
            if (mkt_engine.messages_processed() >= submitted_total) return;
            std::this_thread::yield();
        }
    };

    std::size_t i = 0;
    uint64_t cancel_attempts = 0, new_order_attempts = 0;
    while (i < noise.size()) {
        for (int b = 0; b < kRequoteEvery && i < noise.size(); ++b, ++i) {
            submit_retry(noise[i]);
        }
        wait_caught_up();

        // Cancel our previous quote before posting a new one — a real
        // market maker doesn't accumulate stale resting orders forever.
        // A cancel of an order that already filled is a harmless no-op
        // (options-engine's cancel path handles a nonexistent id
        // gracefully; it does not need this demo to track fill state
        // for that purpose).
        if (current_bid_id != 0) { ++cancel_attempts;
            MarketDataMsg c{}; c.msg_type = MarketDataMsg::Type::CancelOrder;
            c.order_id = current_bid_id; c.symbol = kSymbol;
            submit_retry(c);
        }
        if (current_ask_id != 0) { ++cancel_attempts;
            MarketDataMsg c{}; c.msg_type = MarketDataMsg::Type::CancelOrder;
            c.order_id = current_ask_id; c.symbol = kSymbol;
            submit_retry(c);
        }
        wait_caught_up();

        // Reference price for the quote is last_trade (updated only via
        // drain_reports() -> mm.on_fill(), i.e. only from ExecutionReport
        // data flowing through the engine's own thread-safe SPSC outbound
        // queue) — deliberately NOT mkt_engine.book_for(kSymbol)->
        // best_quote(). Calling best_quote() from this thread while the
        // engine's own matching thread is still running is exactly the
        // hazard MatchingEngine::book_for()'s own doc comment warns
        // about (added earlier, after examples/recovery_demo.cpp hit the
        // same class of bug in options-engine itself): the OrderBook has
        // no lock, and the matching thread writes to it continuously.
        // Confirmed directly here too: an earlier version of this loop
        // read best_quote() every cycle while the engine ran, and TSan
        // caught the exact same data race (order_book.cpp's
        // insert_level/remove_level racing against best_quote()'s
        // reads). Fixed by only ever deriving the reference price from
        // the properly-synchronized report stream, never touching the
        // book's memory directly from this thread while it's live.
        const Price mid = last_trade;

        auto q = mm.compute_quote(mid);
        current_bid_id = current_ask_id = 0;
        if (q.bid != PRICE_INVALID) {
            MarketDataMsg m{};
            m.msg_type = MarketDataMsg::Type::NewOrder; m.order_type = OrderType::Limit;
            m.order_id = next_mm_id; m.symbol = kSymbol; m.side = Side::Buy;
            m.price = q.bid; m.qty = cfg.quote_qty;
            submit_retry(m);
            mm.register_order(next_mm_id, Side::Buy);
            current_bid_id = next_mm_id++; ++new_order_attempts;
        }
        if (q.ask != PRICE_INVALID) {
            MarketDataMsg m{};
            m.msg_type = MarketDataMsg::Type::NewOrder; m.order_type = OrderType::Limit;
            m.order_id = next_mm_id; m.symbol = kSymbol; m.side = Side::Sell;
            m.price = q.ask; m.qty = cfg.quote_qty;
            submit_retry(m);
            mm.register_order(next_mm_id, Side::Sell);
            current_ask_id = next_mm_id++; ++new_order_attempts;
        }
        wait_caught_up();
        if (verbose && (i / kRequoteEvery) % 100 == 0) {
            fprintf(stderr, "[diag] i=%zu position=%lld suppressed=%lu current_bid=%llu current_ask=%llu\n",
                    i, (long long)mm.stats().position, (unsigned long)mm.stats().quote_sides_suppressed_at_limit,
                    (unsigned long long)current_bid_id, (unsigned long long)current_ask_id);
        }
    }

    // Cancel any outstanding quote before final drain — a real market
    // maker withdraws its resting orders on shutdown; leaving them live
    // during shutdown is exactly how the backlog problem described above
    // happens.
    if (current_bid_id != 0) {
        MarketDataMsg c{}; c.msg_type = MarketDataMsg::Type::CancelOrder;
        c.order_id = current_bid_id; c.symbol = kSymbol;
        submit_retry(c);
    }
    if (current_ask_id != 0) {
        MarketDataMsg c{}; c.msg_type = MarketDataMsg::Type::CancelOrder;
        c.order_id = current_ask_id; c.symbol = kSymbol;
        submit_retry(c);
    }
    wait_caught_up();

    // Let the engine finish processing whatever's still queued, then
    // drain the tail of reports — same poll-until-idle discipline as
    // Pipeline::wait_until_drained(), not a fixed sleep guess.
    {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(2000);
        while (clock::now() < deadline) {
            drain_reports();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        drain_reports();
    }

    mkt_engine.stop();

    const auto& s = mm.stats();
    printf("=== Market Maker Demo (synthetic flow, %lu noise events) ===\n", (unsigned long)n_events);
    printf("fills               : %lu\n", (unsigned long)s.fills);
    printf("cancel attempts     : %lu\n", (unsigned long)cancel_attempts);
    printf("new order attempts  : %lu\n", (unsigned long)new_order_attempts);
    printf("final position      : %lld\n", (long long)s.position);
    printf("avg cost (if open)  : %.4f\n", s.position != 0 ? from_price(Price(s.avg_cost_fp)) : 0.0);
    printf("realized P&L        : %.4f\n", from_price(Price(s.realized_pnl_fp)));
    printf("unrealized P&L      : %.4f  (marked at last trade price %.4f)\n",
           from_price(Price(mm.unrealized_pnl(last_trade))), from_price(last_trade));
    printf("quote sides suppressed at position limit: %lu\n", (unsigned long)s.quote_sides_suppressed_at_limit);
    printf("\nNOTE: synthetic, uncalibrated order flow — no real adverse\n");
    printf("selection or informational structure. This exercises the\n");
    printf("mechanics end to end; it is not a claim of profitable alpha.\n");
    printf("See include/hft/market_maker.hpp and docs/strategy-notes.md.\n");
    return 0;
}
