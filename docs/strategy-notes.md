# Strategy notes (quantitative reasoning on this pipeline)

This repo is primarily **infrastructure**. `include/hft/market_maker.hpp` /
`tools/run_market_maker_demo.cpp` / `tests/test_market_maker.cpp` are a small,
honestly-scoped addition on top of it — a real, tested inventory-aware market
maker consuming this pipeline's own `ExecutionReport`s, not a mock. The notes
below are the reasoning it's built from, so a quant-dev interviewer has
something to probe beyond plumbing, and something real to point questions at.

## What actually exists now vs. what's still just reasoning

| | Status |
|---|---|
| Inventory tracking from real fills | **Real, tested** — average-cost P&L, verified exactly on a round trip |
| Correct fill-side attribution (aggressor vs. passive) | **Real, tested** — see market_maker.hpp's header comment on why `ExecutionReport::side` can't be used directly |
| Avellaneda-Stoikov-style inventory skew | **Real, tested** — quote direction verified both long and short |
| Position limit enforcement | **Real, tested** — both in isolation and end-to-end against the live pipeline |
| Calibrated \(\sigma, k, \gamma\) from real data | Not done — config constants are illustrative |
| A live alpha signal / adverse-selection model | Not done |
| A proper backtest (Sharpe, drawdown, etc.) | Not done |

## Objects from the stack

- Software tick-to-trade samples: time from `recv_ns` (or submit stamp) to fill observed
- Fill stream: `ExecutionReport` with price, qty, side — the actual input `MarketMaker::on_fill()` consumes
- Book state: **not read directly by the market maker at all.** An early
  version of `run_market_maker_demo.cpp` called `book_for(symbol)->
  best_quote()` from the demo's own thread while the engine's matching
  thread was still running — exactly the hazard this line already warned
  about before that file existed, and TSan caught it directly the first
  time the tool was actually run under a sanitizer. Fixed by deriving the
  quote's reference price only from `last_trade`, updated exclusively via
  the engine's own thread-safe `ExecutionReport` stream — never touching
  `OrderBook`'s memory from a second thread while it's live.

## Inventory-skew intuition (Avellaneda–Stoikov style, simplified)

Let inventory \(q\) be net shares (long positive). A minimal reservation price:

\[
r = s - q \gamma \sigma^2 (T-t)
\]

and half-spread roughly:

\[
\delta \approx \frac{\gamma\sigma^2(T-t)}{2} + \frac{1}{\gamma}\ln\left(1+\frac{\gamma}{k}\right)
\]

where \(s\) is mid, \(\sigma\) is volatility, \(\gamma\) risk aversion, \(k\) order-arrival intensity.

`MarketMaker::compute_quote()` implements a stationary, infinite-horizon
simplification of this — no explicit \((T-t)\) term, a single linear
`inventory_skew` constant standing in for \(\gamma\sigma^2\), and a fixed
`half_spread` rather than the calibrated \(\delta\) above. That's a real,
disclosed simplification, not a full implementation of the model — see
market_maker.hpp for exactly what's simplified and why.

**Link to this pipeline:**

1. **Latency \(\tau\)** (software TTT under load) delays both quote updates and cancel/replace.
   Adverse selection risk grows with \(\tau\): informed flow that arrives within \(\tau\) of a
   mid move hits stale quotes. Rough cost scale: \(\sim \sigma\sqrt{\tau}\) per unit size
   in a diffusion caricature — not a calibrated model, a dimensional check.

2. **Queue position / fill probability** depends on join time vs competing flow.
   If your cancel latency is \(\tau_c\) and the book depletes in \(\tau_d\), you need
   \(\tau_c \ll \tau_d\) for risk controls to matter. Burst software-path \(\tau\) of
   milliseconds (shared core) is **not** competitive for top-of-book HFT; isolated
   cores and kernel-bypass are the next engineering steps, not more strategy math.
   `run_market_maker_demo.cpp` hit a concrete instance of this directly: on
   this sandbox's single core, the engine's processing lagged submission
   badly enough that a large backlog of already-posted quotes could all
   match well after the quote-time position-limit check had any chance to
   see it happening — a real demonstration that quote-time-only risk
   checks don't protect against orders that are already resting, not just
   a textbook caveat.

3. **Inventory from ExecutionReports:** after each fill, \(q \leftarrow q \pm \text{qty}\).
   Skew quotes: lower reservation when \(q>0\) (eager to sell), raise when \(q<0\).
   This is the one piece that's now real code, not just the equation above
   — see `tests/test_market_maker.cpp`'s skew-direction tests.

## What this stack does *not* claim

- No calibrated \(\sigma, k, \gamma\) — `half_spread`/`inventory_skew` are
  illustrative constants, tuned only so the demo's quotes fall inside its
  own synthetic feed's price range (see run_market_maker_demo.cpp's
  comment on this — a first run with an untuned default posted zero fills
  for a mundane, disclosed reason: the spread was simply wider than the
  synthetic feed's entire trading range).
- No live alpha signal, no adverse-selection model
- Synthetic stream \(\neq\) historical microstructure — any P&L number
  `run_market_maker_demo.cpp` prints reflects whether the *mechanics* are
  implemented correctly against a synthetic, uncalibrated stream with no
  real informational structure, not whether the strategy would be
  profitable against a real market. Do not read it as an alpha claim.

## What to say in interview

> The integration proves correct, measured plumbing. On top of it,
> MarketMaker is a real, tested component: correct average-cost P&L,
> correct fill-side attribution (including the passive-fill case that's
> easy to get backwards), a working inventory skew, and an enforced
> position limit — verified both in isolated unit tests and end-to-end
> against the live matching engine, including a real data race I found
> and fixed while wiring it up. It is not a trading strategy with alpha;
> I won't pretend a synthetic fill log is a Sharpe number. Building a
> real backtest against real data is a separate, larger piece of work.
