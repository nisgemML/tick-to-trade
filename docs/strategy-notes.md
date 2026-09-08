# Strategy notes (quantitative reasoning on this pipeline)

This repo is **infrastructure**. The notes below are not a production strategy —
they show how measured pipeline properties would enter a simple market-making
decision, so quant-dev interviewers have something to probe beyond plumbing.

## Objects from the stack

- Software tick-to-trade samples: time from `recv_ns` (or submit stamp) to fill observed
- Fill stream: `ExecutionReport` with price, qty, side
- Book state: available via matching engine after stop (not concurrent-safe while running)

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

3. **Inventory from ExecutionReports:** after each fill, \(q \leftarrow q \pm \text{qty}\).
   Skew quotes: lower reservation when \(q>0\) (eager to sell), raise when \(q<0\).

## What this stack does *not* claim

- No calibrated \(\sigma, k, \gamma\)
- No live alpha signal
- Synthetic stream \(\neq\) historical microstructure

## What to say in interview

> The integration proves correct, measured plumbing. Strategy notes show how I’d
> fold measured latency into inventory risk. Building a full backtest is a
> separate artifact; I won’t pretend a synthetic fill log is a Sharpe number.

