#!/usr/bin/env python3
"""
calibrate_mm.py — derive MarketMaker::inventory_skew from real market data.

Run this on a machine that can actually reach exchange APIs (this script
cannot run inside the sandbox this repo was built in — that environment's
network egress is allowlisted to package registries and GitHub only;
confirmed directly, not assumed: both a raw curl and Anthropic's own
web_fetch tool were tried against api.binance.com and both were refused).

WHAT THIS DOES: fetches real recent 1-minute klines for a liquid
instrument from Binance's public (no-auth-required) REST API, computes
realized volatility from actual log returns, and prints a derived
inventory_skew value plus the exact C++ config line to use it.

WHAT THIS DOES NOT DO: calibrate k (order-arrival intensity) or provide
a genuine T-t horizon — MarketMaker::compute_quote() is still the
stationary, infinite-horizon simplification documented in
market_maker.hpp and docs/strategy-notes.md. This script closes ONE gap
(sigma is now measured, not guessed) — it does not turn this into a
calibrated trading strategy. gamma (risk aversion) below is still a
CHOICE, not something derivable from price data alone; it's set to a
commonly-cited illustrative value from the Avellaneda-Stoikov literature,
labeled as exactly that.

Usage:
    python3 calibrate_mm.py [SYMBOL] [MINUTES] [TARGET_PRICE]

    python3 calibrate_mm.py BTCUSDT 1440        # last 24h, applied to $100 (default)
    python3 calibrate_mm.py BTCUSDT 1440 100.0  # same, explicit
    python3 calibrate_mm.py ETHUSDT 500 50.0    # different source + target price

TARGET_PRICE matters and is not cosmetic: it's the price level the
measured RELATIVE volatility gets applied to, which should match
whatever instrument you're actually quoting (run_market_maker_demo.cpp's
synthetic feed defaults to $100 — see include/hft/feed.hpp's
SyntheticFeedConfig::mid), not the source instrument's own price. Do NOT
skip this and assume it's fine to default — confirmed directly: applying
BTC's own ~$50k price level to itself, then using that inventory_skew
against a $100 demo, produces a reservation-price shift of order
thousands of dollars per 100 units of inventory, against a demo whose
entire price range is about a cent. Relative (percentage) volatility is
roughly comparable across price scales; dollar volatility is not.
"""
import sys
import json
import math
import statistics
import urllib.request
import urllib.error
from datetime import datetime, timezone

BINANCE_COM_KLINES = "https://api.binance.com/api/v3/klines"
BINANCE_US_KLINES  = "https://api.binance.us/api/v3/klines"
# Binance.com (the international exchange) returns HTTP 451 for
# US-based requests, per its own terms — confirmed directly, not
# hypothetical: this script's first real run, from a US-based machine,
# got exactly that. Binance.US is a separate, US-regulated exchange
# with its own API — but one that mirrors the same /api/v3/klines
# endpoint and response format ("shares an API dialect with Binance",
# per CCXT's own comparison of the two), so no parsing logic below
# needs to differ, only the base URL. This tries .com first (works for
# anyone outside the US) and falls back to .us automatically on a 451,
# rather than requiring the caller to know in advance which one their
# location needs.

# Avellaneda-Stoikov's own worked example uses gamma=0.1 as an
# illustrative risk-aversion constant — not derived from data, a stated
# choice. Kept here as a labeled default, not hidden inside the formula.
DEFAULT_GAMMA = 0.1


def _fetch_from(base_url: str, symbol: str, limit: int):
    url = f"{base_url}?symbol={symbol}&interval=1m&limit={limit}"
    req = urllib.request.Request(url, headers={"User-Agent": "calibrate_mm.py"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read().decode())


def fetch_klines(symbol: str, minutes: int):
    """Fetch up to `minutes` 1-minute klines. Binance caps a single
    request at 1000 candles; this script does not paginate beyond that
    (i.e. minutes > 1000 silently gets the most recent 1000 only) —
    deliberately, since going further starts to matter for what
    "recent realized volatility" even means, and that's a judgment call
    this script should not make silently for you."""
    limit = min(minutes, 1000)
    try:
        data = _fetch_from(BINANCE_COM_KLINES, symbol, limit)
    except urllib.error.HTTPError as e:
        if e.code == 451:
            print("api.binance.com returned 451 (geo-restricted for this "
                  "location) — retrying against api.binance.us...")
            try:
                data = _fetch_from(BINANCE_US_KLINES, symbol, limit)
            except urllib.error.HTTPError as e2:
                body = e2.read().decode(errors="replace")
                sys.exit(f"HTTP {e2.code} fetching {symbol} from Binance.US "
                          f"too: {body}\n(Binance.US carries ~150 pairs, "
                          f"a subset of Binance.com's ~1,500+ — this symbol "
                          f"may simply not be listed there. Try BTCUSDT or "
                          f"ETHUSDT, both listed on both.)")
            except urllib.error.URLError as e2:
                sys.exit(f"Could not reach Binance.US either: {e2.reason}")
        else:
            body = e.read().decode(errors="replace")
            sys.exit(f"HTTP {e.code} fetching {symbol}: {body}\n"
                      f"(Common cause: symbol doesn't exist on Binance spot — "
                      f"try BTCUSDT, ETHUSDT, etc.)")
    except urllib.error.URLError as e:
        sys.exit(f"Could not reach Binance: {e.reason}\n"
                  f"This script needs to run somewhere with real internet "
                  f"access — it will not work inside a sandboxed dev "
                  f"environment with restricted network egress.")
    if limit < minutes:
        print(f"NOTE: requested {minutes} minutes, Binance's per-request "
              f"cap is 1000 — using the most recent {limit} only.\n")
    return data


def realized_vol(klines):
    """Realized volatility of 1-minute log returns, both per-minute and
    annualized (assuming ~525,600 minutes/year, the standard crypto
    convention since these markets trade 24/7 — NOT the ~98,280-minute
    equity-market-hours convention, which matters if you re-point this
    at a different asset class later)."""
    closes = [float(k[4]) for k in klines]
    if len(closes) < 2:
        sys.exit("Not enough klines returned to compute a return series.")
    log_returns = [math.log(closes[i] / closes[i - 1]) for i in range(1, len(closes))]
    sigma_per_min = statistics.pstdev(log_returns)
    sigma_annual = sigma_per_min * math.sqrt(525_600)
    mean_price = statistics.mean(closes)
    return sigma_per_min, sigma_annual, mean_price, closes[-1]


def main():
    symbol = sys.argv[1] if len(sys.argv) > 1 else "BTCUSDT"
    minutes = int(sys.argv[2]) if len(sys.argv) > 2 else 1440
    # The price level to APPLY the calibration to — i.e. what
    # run_market_maker_demo.cpp's synthetic feed actually trades at
    # (SyntheticFeedConfig::mid defaults to $100 — see include/hft/feed.hpp),
    # NOT the source instrument's own price. This is not a cosmetic
    # detail: relative (percentage) volatility is roughly comparable
    # across price scales, but DOLLAR volatility is not — confirmed
    # directly by computing both ways: naively applying BTC's own ~$50k
    # price level would derive an inventory_skew implying a ~$6,250
    # reservation-price shift per 100 units of inventory, against a
    # synthetic demo whose entire price range is about $0.01. Applying
    # the SAME measured relative sigma to the demo's actual $100 price
    # level instead gives a sane, usable result.
    target_price = float(sys.argv[3]) if len(sys.argv) > 3 else 100.0

    print(f"Fetching {minutes} x 1-minute klines for {symbol} from Binance "
          f"(public API, no auth)...")
    klines = fetch_klines(symbol, minutes)
    n = len(klines)
    t_start = datetime.fromtimestamp(klines[0][0] / 1000, tz=timezone.utc)
    t_end = datetime.fromtimestamp(klines[-1][0] / 1000, tz=timezone.utc)
    print(f"Got {n} bars, {t_start.isoformat()} to {t_end.isoformat()}\n")

    sigma_min, sigma_annual, mean_price, last_price = realized_vol(klines)

    print(f"=== Realized volatility, {symbol}, real data ===")
    print(f"  mean price (window)   : {mean_price:.4f}")
    print(f"  last price            : {last_price:.4f}")
    print(f"  sigma (per 1-min bar) : {sigma_min:.6f}  ({sigma_min*100:.4f}% per minute, RELATIVE)")
    print(f"  sigma (annualized)    : {sigma_annual:.4f}  ({sigma_annual*100:.2f}%/yr, RELATIVE)")

    # inventory_skew stands in for gamma*sigma^2 in the stationary
    # simplification MarketMaker::compute_quote() actually implements
    # (see market_maker.hpp's own header comment on exactly what's
    # simplified away — no T-t term here, same as there). sigma_min is a
    # RELATIVE return volatility (dimensionless, comparable across price
    # scales); MarketMaker::compute_quote() wants a DOLLAR price-shift per
    # unit of inventory AT THE PRICE LEVEL BEING QUOTED — this applies the
    # measured relative sigma to target_price, deliberately NOT to
    # last_price (the source instrument's own price), for exactly the
    # reason in this function's leading comment.
    sigma_dollars = sigma_min * target_price
    gamma = DEFAULT_GAMMA
    inventory_skew = gamma * (sigma_dollars ** 2)

    print(f"\n=== Applying to target price level ${target_price:.2f} "
          f"(pass a 3rd argument to change) ===")
    print(f"  gamma (risk aversion) : {gamma}  <- CHOICE, not derived from data;")
    print(f"                                     this is Avellaneda-Stoikov's own")
    print(f"                                     illustrative constant, not fit to")
    print(f"                                     anything here.")
    print(f"  sigma applied at ${target_price:.2f} : {sigma_dollars:.8f} dollars/bar")
    print(f"  inventory_skew        : {inventory_skew:.10f}")
    print(f"\nDrop this into MarketMakerConfig (include/hft/market_maker.hpp):\n")
    print(f"    cfg.inventory_skew = {inventory_skew:.10f};  // gamma={gamma} (AS illustrative")
    print(f"                                       // const) x sigma^2, sigma measured from")
    print(f"                                       // {n} real 1-min {symbol} bars,")
    print(f"                                       // {t_start.date()} to {t_end.date()},")
    print(f"                                       // relative sigma applied at this repo's")
    print(f"                                       // own ${target_price:.2f} price level —")
    print(f"                                       // see calibrate_mm.py for why NOT to")
    print(f"                                       // use {symbol}'s own ${last_price:,.2f}")
    print(f"                                       // price level directly.")

    print(f"\n=== What this does and does not close ===")
    print(f"  CLOSED : inventory_skew is now measured from real, dated market data,")
    print(f"           not an arbitrary illustrative constant — AND correctly rescaled")
    print(f"           to the price level actually being quoted, not naively copied")
    print(f"           from the source instrument's own (very different) price scale.")
    print(f"  NOT CLOSED : half_spread, k (order-arrival intensity), and the T-t")
    print(f"           horizon term remain unmodeled/uncalibrated — this measures")
    print(f"           sigma, nothing else. gamma remains a stated preference, not")
    print(f"           something any amount of price data alone determines.")
    print(f"  NOTE ON MATCHING: the matching itself still runs against this repo's")
    print(f"           synthetic order flow, not this real price series — this")
    print(f"           script informs ONE config constant, it does not replay real")
    print(f"           market data through the matching engine.")
    print(f"\n  For equities instead of crypto: Binance doesn't carry them. Free")
    print(f"  no-auth alternatives exist (e.g. stooq.com's daily CSV endpoints) but")
    print(f"  daily bars give you a much noisier, lower-resolution sigma estimate")
    print(f"  than the 1-minute crypto bars this script uses by default.")


if __name__ == "__main__":
    main()
