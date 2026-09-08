#pragma once
#include "core/types.hpp"
#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace hft {

// A minimal, honestly-scoped inventory-aware market maker.
//
// ── What this demonstrates ──────────────────────────────────────────────
//
// Correct inventory tracking from real ExecutionReports (not synthetic
// bookkeeping), correct average-cost P&L accounting (verified exactly on
// a round trip: buy N @ P1 then sell N @ P2 must realize exactly
// N*(P2-P1) — see tests/test_market_maker.cpp), and an
// Avellaneda-Stoikov-style inventory skew: the reservation price moves
// opposite to inventory, so a market maker that is long skews its quotes
// down (more eager to sell what it already holds, less eager to buy
// more) and one that is short skews up. This is the same core intuition
// docs/strategy-notes.md reasons about — now actually running against
// real fills from this pipeline's own MatchingEngine, not only reasoned
// about on paper.
//
// This is a stationary, infinite-horizon simplification of Avellaneda-
// Stoikov (no explicit time-to-close-of-session term, no calibrated
// volatility/risk-aversion parameters) — the config's inventory_skew and
// half_spread are illustrative constants, not values fit to real data.
//
// ── What this does NOT claim ────────────────────────────────────────────
//
// This is not a source of alpha. It runs against a synthetic,
// uncalibrated order flow (see hft::make_synthetic_stream) with no real
// microstructure signal — no genuine adverse selection to protect
// against, no real informational edge to exploit. Any P&L this produces
// against that synthetic stream reflects whether the mechanics are
// implemented correctly, not whether the underlying strategy would be
// profitable against a real market. Do not read a positive P&L number
// from tools/run_market_maker_demo.cpp as evidence of anything beyond
// "the accounting is exercised end-to-end and didn't crash."
//
// ── A correctness detail worth being explicit about ─────────────────────
//
// options-engine's matching engine fires exactly ONE ExecutionReport per
// match, for the AGGRESSOR's order (order_id = the incoming/aggressive
// order, contra_order_id = the resting/passive order, side = the
// AGGRESSOR's side — see order_book.cpp's match loop). A market maker's
// entire purpose is to REST orders and wait to be hit, meaning most of
// its own fills will have ITS order as contra_order_id, not order_id —
// and rpt.side in that case is the COUNTERPARTY's side, the opposite of
// what this market maker actually traded. Using rpt.side directly would
// silently record every passive fill backwards. register_order() tracks
// each of our own orders' actual side ourselves; on_fill() looks up
// whichever of order_id/contra_order_id is one of ours and uses THAT
// order's own recorded side, never rpt.side, for P&L purposes.

struct MarketMakerConfig {
    engine::Price half_spread    = engine::to_price(0.02);  // base half-spread around reservation price
    double        inventory_skew = 0.0001;                  // price shift ($) per unit of signed inventory
    int64_t       position_limit = 500;                     // hard cap on |position|
    engine::Qty   quote_qty      = 100;                      // size posted on each side
};

struct MarketMakerStats {
    int64_t  position        = 0;   // signed inventory: +long, -short
    int64_t  avg_cost_fp     = 0;   // fixed-point average cost of current position (0 if flat)
    int64_t  realized_pnl_fp = 0;   // fixed-point, same scale as engine::Price (price * 10^6)
    uint64_t fills           = 0;
    uint64_t quote_sides_suppressed_at_limit = 0;
};

struct Quote {
    engine::Price bid = engine::PRICE_INVALID;
    engine::Price ask = engine::PRICE_INVALID;
};

class MarketMaker {
public:
    MarketMaker(engine::SymbolId symbol, MarketMakerConfig cfg, uint32_t account_id)
        : symbol_(symbol), cfg_(cfg), account_id_(account_id) {}

    engine::SymbolId symbol() const { return symbol_; }
    uint32_t account_id() const { return account_id_; }

    // Compute the quote to post around a reference (mid) price. Either
    // side is independently suppressed (PRICE_INVALID) if posting
    // cfg_.quote_qty more on that side could breach position_limit —
    // this is checked against the CURRENT position, i.e. it assumes the
    // posted quote might fully fill; it does not attempt to predict
    // whether it actually will.
    [[nodiscard]] Quote compute_quote(engine::Price mid) {
        const double skew_dollars = double(stats_.position) * cfg_.inventory_skew;
        const engine::Price reservation = mid - engine::to_price(skew_dollars);

        Quote q;
        const int64_t qty = int64_t(cfg_.quote_qty);
        if (stats_.position + qty <= cfg_.position_limit) {
            q.bid = reservation - cfg_.half_spread;
        } else {
            ++stats_.quote_sides_suppressed_at_limit;
        }
        if (stats_.position - qty >= -cfg_.position_limit) {
            q.ask = reservation + cfg_.half_spread;
        } else {
            ++stats_.quote_sides_suppressed_at_limit;
        }
        return q;
    }

    // Record that we submitted an order, so a later fill referencing it
    // (as either order_id or contra_order_id) can be attributed to the
    // side WE actually traded — see this file's header comment for why
    // that must not be read from ExecutionReport::side directly.
    void register_order(engine::OrderId id, engine::Side side) {
        open_orders_[id] = side;
    }

    // Feed a real ExecutionReport. Returns true if it was attributed to
    // one of our own tracked orders (false means it belongs to some
    // other participant entirely and this call had no effect).
    bool on_fill(const engine::ExecutionReport& rpt) {
        auto it = open_orders_.find(rpt.order_id);
        engine::OrderId matched_id = rpt.order_id;
        if (it == open_orders_.end()) {
            it = open_orders_.find(rpt.contra_order_id);
            matched_id = rpt.contra_order_id;
        }
        if (it == open_orders_.end()) return false;

        apply_fill(it->second, rpt.exec_price, rpt.exec_qty);
        ++stats_.fills;
        if (rpt.leaves_qty == 0) open_orders_.erase(matched_id);
        return true;
    }

    [[nodiscard]] int64_t unrealized_pnl(engine::Price mark_price) const {
        if (stats_.position == 0) return 0;
        return stats_.position * (int64_t(mark_price) - stats_.avg_cost_fp);
    }

    [[nodiscard]] const MarketMakerStats& stats() const { return stats_; }

private:
    // Weighted-average-cost P&L: buying extends/opens a position at a
    // blended average price; selling against an existing position
    // realizes P&L against that average. This is the standard
    // average-cost convention (not FIFO/LIFO lot matching) — chosen
    // because the result is independent of fill ORDER within a position,
    // which makes it exactly, unambiguously verifiable on a round trip
    // regardless of how many separate fills built up either side of it.
    // Position, avg_cost, and realized_pnl are all int64_t throughout —
    // engine::Qty is uint32_t (unsigned), and this arithmetic genuinely
    // needs to go negative (short positions, closing trades), so Qty is
    // only ever converted to int64_t at the point a fill's magnitude
    // enters this function, never used directly in a signed comparison.
    void apply_fill(engine::Side side, engine::Price price, engine::Qty qty) {
        const int64_t signed_qty = (side == engine::Side::Buy) ? int64_t(qty) : -int64_t(qty);
        const int64_t old_position = stats_.position;

        if (old_position == 0 || (old_position > 0) == (signed_qty > 0)) {
            // Opening or extending in the same direction: blend into
            // the average cost. No P&L realized yet.
            const int64_t new_position = old_position + signed_qty;
            stats_.avg_cost_fp =
                (stats_.avg_cost_fp * old_position + int64_t(price) * signed_qty) / new_position;
            stats_.position = new_position;
            return;
        }

        // Reducing (or flipping through zero): realize P&L on the
        // portion that offsets the existing position, against its
        // current average cost.
        const int64_t closing_qty = std::min<int64_t>(std::abs(signed_qty), std::abs(old_position));
        const int64_t direction   = (old_position > 0) ? 1 : -1;
        stats_.realized_pnl_fp += direction * closing_qty * (int64_t(price) - stats_.avg_cost_fp);
        stats_.position = old_position + signed_qty;

        if (stats_.position == 0) {
            stats_.avg_cost_fp = 0;
        } else if ((stats_.position > 0) != (direction > 0)) {
            // Flipped through zero: the remaining quantity opens a
            // brand-new position on the OPPOSITE side, at this fill's
            // price (there is no prior average cost for that side).
            stats_.avg_cost_fp = int64_t(price);
        }
        // else: still the same sign, just reduced — avg_cost_fp is
        // unchanged, which is correct for average-cost accounting: the
        // cost basis of what remains doesn't change when you sell part
        // of it.
    }

    engine::SymbolId   symbol_;
    MarketMakerConfig  cfg_;
    uint32_t           account_id_;
    MarketMakerStats   stats_{};
    std::unordered_map<engine::OrderId, engine::Side> open_orders_;
};

} // namespace hft
