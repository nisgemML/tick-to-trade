# Interview one-pager

## What this answers

**Systems / low-latency infra:** composition, back-pressure, gap handling, measurement honesty, sanitizers.

**Quant developer (signal/pricing):** this repo is plumbing only — no alpha, pricing, or risk model. Pair with a separate quantitative project if the role weights research heavily.

## Pitch (45s)

Four owned components composed into one path:

MoldUDP/ITCH → GapBuffer → MarketDataMsg → MatchingEngine → log

Tests use NDEBUG-safe CHECK (Release CI proves failures still fail). Gap injection is tested. Bench no longer times a fixed sleep.

## Open live

1. `tests/test_gap_injection.cpp`
2. `tests/test_backpressure.cpp`
3. `bench/bench_pipeline.cpp` (submit vs e2e split)
4. `BUGS_FOUND.md`
5. `.github/workflows/ci.yml` (Release + ASan + TSan)
