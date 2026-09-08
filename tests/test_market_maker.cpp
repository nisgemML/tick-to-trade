#include "hft/check.hpp"
#include "hft/market_maker.hpp"
#include <cstdio>

using engine::Side;
using engine::ExecutionReport;
using engine::ExecType;

static ExecutionReport make_report(engine::OrderId order_id, engine::OrderId contra_id,
                                    engine::Price price, engine::Qty qty,
                                    engine::Qty leaves, Side aggressor_side) {
    ExecutionReport r{};
    r.order_id = order_id;
    r.contra_order_id = contra_id;
    r.exec_price = price;
    r.exec_qty = qty;
    r.leaves_qty = leaves;
    r.symbol = 1;
    r.side = aggressor_side;  // deliberately the AGGRESSOR's side — see
                              // market_maker.hpp's header comment; a
                              // correct MarketMaker must NOT use this
                              // field to determine its own side.
    r.exec_type = (leaves == 0) ? ExecType::Fill : ExecType::PartialFill;
    return r;
}

int main() {
    // ── 1. Exact round-trip P&L, aggressor side ─────────────────────────
    {
        hft::MarketMaker mm(1, {}, 42);
        mm.register_order(100, Side::Buy);
        // We are the aggressor (order_id == our order): buy 100 @ $10.00
        bool ok1 = mm.on_fill(make_report(100, /*contra*/ 900, engine::to_price(10.00), 100, 0, Side::Buy));
        CHECK(ok1, "buy fill attributed to our order");
        CHECK(mm.stats().position == 100, "position is +100 after buying 100");
        CHECK(mm.stats().avg_cost_fp == engine::to_price(10.00), "avg cost is 10.00 after a single buy");

        mm.register_order(101, Side::Sell);
        bool ok2 = mm.on_fill(make_report(101, /*contra*/ 901, engine::to_price(10.50), 100, 0, Side::Sell));
        CHECK(ok2, "sell fill attributed to our order");
        CHECK(mm.stats().position == 0, "flat after selling exactly what we bought");
        const int64_t expected_pnl = int64_t(100) * (engine::to_price(10.50) - engine::to_price(10.00));
        CHECK(mm.stats().realized_pnl_fp == expected_pnl,
              "realized P&L is EXACTLY 100 * (10.50 - 10.00), not approximately");
    }

    // ── 2. Fill attribution when we are the PASSIVE (contra) side ───────
    // This is the case that matters most for a market maker — its orders
    // rest and get hit — and the case where using rpt.side directly
    // (the AGGRESSOR's side) would silently record the fill backwards.
    {
        hft::MarketMaker mm(1, {}, 42);
        mm.register_order(500, Side::Sell);  // we rested a sell
        // Someone else aggressively BUYS into our resting sell. rpt.side
        // is Buy (the aggressor's side) — but WE sold.
        bool ok = mm.on_fill(make_report(/*order_id=aggressor*/ 777, /*contra=*/ 500,
                                          engine::to_price(20.00), 50, 0, Side::Buy));
        CHECK(ok, "fill attributed via contra_order_id");
        CHECK(mm.stats().position == -50,
              "position is -50 (we SOLD) even though rpt.side says Buy — "
              "using rpt.side directly here would have wrongly recorded +50");
    }

    // ── 3. Unrelated fill is correctly ignored ───────────────────────────
    {
        hft::MarketMaker mm(1, {}, 42);
        bool ok = mm.on_fill(make_report(1, 2, engine::to_price(1.0), 1, 0, Side::Buy));
        CHECK(!ok, "a fill referencing no order we registered returns false");
        CHECK(mm.stats().position == 0, "and has no effect on position");
    }

    // ── 4. Quote skew direction ──────────────────────────────────────────
    {
        hft::MarketMakerConfig cfg;
        cfg.inventory_skew = 0.001;
        hft::MarketMaker mm(1, cfg, 42);
        const engine::Price mid = engine::to_price(100.00);

        auto flat_quote = mm.compute_quote(mid);
        CHECK(flat_quote.bid != engine::PRICE_INVALID && flat_quote.ask != engine::PRICE_INVALID,
              "flat position quotes both sides");
        const engine::Price flat_mid_of_quote = (flat_quote.bid + flat_quote.ask) / 2;
        CHECK(flat_mid_of_quote == mid, "at zero inventory, the quote is symmetric around mid");

        mm.register_order(1, Side::Buy);
        mm.on_fill(make_report(1, 2, mid, 300, 0, Side::Buy));  // now long 300
        auto long_quote = mm.compute_quote(mid);
        const engine::Price long_mid_of_quote = (long_quote.bid + long_quote.ask) / 2;
        CHECK(long_mid_of_quote < mid,
              "long position skews the reservation price BELOW mid — more eager to sell");

        hft::MarketMaker mm2(1, cfg, 42);
        mm2.register_order(1, Side::Sell);
        mm2.on_fill(make_report(1, 2, mid, 300, 0, Side::Sell));  // now short 300
        auto short_quote = mm2.compute_quote(mid);
        const engine::Price short_mid_of_quote = (short_quote.bid + short_quote.ask) / 2;
        CHECK(short_mid_of_quote > mid,
              "short position skews the reservation price ABOVE mid — more eager to buy");
    }

    // ── 5. Position limit is actually enforced ───────────────────────────
    {
        hft::MarketMakerConfig cfg;
        cfg.position_limit = 200;
        cfg.quote_qty = 100;
        hft::MarketMaker mm(1, cfg, 42);
        const engine::Price mid = engine::to_price(50.00);

        // Push position to +150 (still room for one more 100-lot buy? No —
        // 150 + 100 = 250 > 200, so bid should already be suppressed).
        mm.register_order(1, Side::Buy);
        mm.on_fill(make_report(1, 2, mid, 150, 0, Side::Buy));
        CHECK(mm.stats().position == 150, "position is 150 after this fill");

        auto q = mm.compute_quote(mid);
        CHECK(q.bid == engine::PRICE_INVALID,
              "bid suppressed: quoting cfg.quote_qty more would breach position_limit (150+100 > 200)");
        CHECK(q.ask != engine::PRICE_INVALID,
              "ask still posted: selling more reduces position, doesn't breach the limit");
    }

    // ── 6. Position flip through zero: correct P&L and new avg cost ─────
    {
        hft::MarketMaker mm(1, {}, 42);
        mm.register_order(1, Side::Buy);
        mm.on_fill(make_report(1, 2, engine::to_price(10.00), 100, 0, Side::Buy));  // long 100 @ 10.00
        CHECK(mm.stats().position == 100, "long 100");

        mm.register_order(3, Side::Sell);
        // Sell 150 — closes the long 100 (realizing P&L) and opens a new
        // short 50 at this fill's price.
        mm.on_fill(make_report(3, 4, engine::to_price(11.00), 150, 0, Side::Sell));
        CHECK(mm.stats().position == -50, "flipped to short 50");
        CHECK(mm.stats().avg_cost_fp == engine::to_price(11.00),
              "new short position's cost basis is this fill's price, not the old long's");
        const int64_t expected = int64_t(100) * (engine::to_price(11.00) - engine::to_price(10.00));
        CHECK(mm.stats().realized_pnl_fp == expected,
              "P&L realized only on the 100 that closed the long, at the correct prices");
    }

    printf("=== hft::MarketMaker tests ===\n");
    TEST_EXIT();
}
