# Benchmark results

## Policy

| Metric | Meaning |
|--------|---------|
| `throughput_submit` | Enqueue rate into MatchingEngine SPSC only (drain concurrent) |
| `throughput_e2e` | Submit + poll-until-drained (no fixed sleep) |
| Software tick-to-trade | `recv_ns` at submit → fill observed on drain thread |
| **Not claimed** | Exchange wire / NIC HW timestamp latency |

## Environment

```
Host: shared container (unpinned)
Build: Release, g++ 13
Date: 2026-09-08
```

## Pipeline (`bench_pipeline 30000`)

| Metric | Value |
|--------|-------|
| events | 30000 |
| fills | 23938 |
| submit_only_us | ~314 µs |
| submit+drain_us | ~1.0 s (poll until drained) |
| throughput_submit | ~95M msg/s (queue push rate) |
| throughput_e2e | ~29k msg/s |
| queue_full | 0 |

Earlier builds timed a fixed 200ms sleep inside the measured region and under-reported throughput. That is fixed: submit and drain are timed separately.

## Correctness (Release, CHECK not assert)

| Test | Result |
|------|--------|
| backpressure | accepted=65535 rejected=4465 |
| conservation | identical fills across runs |
| feed_adapter | price scale ×100 checked |
| gap_injection | gap fires; seq 2/3 deliver; duplicate dropped |
| check_macro_fails | exits 1 under Release |

## NUMA/hugepage-backed GapBuffer under realistic gap-recovery load

Does `-DHFT_WITH_NUMA=ON` (BUGS_FOUND.md #14) actually change anything
measurable, in the pipeline's own real usage pattern — not a generic
microbenchmark on an arbitrary payload? `bench/bench_gapbuffer_numa.cpp`
feeds `GapBuffer` 200 laps of 4096 sequence numbers each, *randomly
permuted within each lap* before being ingested one at a time — forcing
scattered writes across the full ~6MB `slots_` array (`seq & kMask`
indexing) the way heavy real out-of-order delivery would, rather than
the sequential, localized access pattern normal in-order delivery
produces. 817,116 messages delivered identically in every run below —
same correctness, only allocation strategy differs.

**First attempt — a misleading result, caught and corrected rather than
published.** The very first run of each binary showed a dramatic gap:

```
default (first run):  p50= 106.7ns  p90=128.5ns  p99=143.0ns
NUMA    (first run):  p50=  78.7ns  p90= 84.5ns  p99=103.8ns   (26-34% faster)
```

That looked great — and was wrong to trust. Three more runs each showed
the gap narrowing and, in one case, reversing (NUMA briefly *slower*).
The first-run numbers were a cold-cache/cold-TLB artifact specific to
being each binary's very first invocation, not a property of the
allocation strategy. Publishing that first comparison would have been
the same mistake as any other single-sample benchmark claim this
portfolio has specifically tried not to make.

**Five repeated, warmed-up runs per configuration instead:**

```
default (regular heap), 5 runs:
  p50=75.9ns  p90=87.2ns  p99=106.3ns
  p50=80.3ns  p90=94.9ns  p99=125.5ns
  p50=81.0ns  p90=86.8ns  p99= 99.8ns
  p50=81.1ns  p90=89.7ns  p99=105.4ns
  p50=79.4ns  p90=86.7ns  p99=103.8ns

NUMA (hugepage-backed), 5 runs:
  p50=75.7ns  p90=82.1ns  p99= 92.2ns
  p50=77.8ns  p90=83.1ns  p99= 92.7ns
  p50=72.4ns  p90=78.7ns  p99= 92.3ns
  p50=72.7ns  p90=78.9ns  p99= 88.3ns
  p50=73.6ns  p90=80.8ns  p99= 89.7ns
```

**The honest, stable finding:** NUMA/hugepage wins every single run at
every percentile — a smaller, more credible effect than the misleading
first-run number, but a real and consistent one: roughly 6-9% faster at
p50/p90 (~79.5ns → ~74.4ns median p50 across the 5 runs; ~89.1ns →
~80.7ns median p90), and a more pronounced ~15-20% at p99 (~108ns →
~91ns). The larger tail-percentile effect is consistent with the
underlying mechanism: TLB-miss cost shows up most under the scattered,
worst-case access patterns p99 captures, not the typical-case access p50
reflects.

**What this is, and isn't:** a real, repeatable, honest measurement of
allocation strategy mattering for this specific scattered-access
workload, on this single-core, single-NUMA-node sandbox. It is not a
measurement of cross-NUMA-node latency (no second node exists here to
measure), and the magnitude may not hold at a different scale, access
pattern, or on real multi-socket hardware — those would need to be
measured separately, not assumed from this number.

## Real NASDAQ data: Nasdaq BX, 2019-12-30

`20191230.BX_ITCH_50.gz` from NASDAQ's public sample server (emi.nasdaq.com): 390,561,039 bytes compressed, matching NASDAQ's directory listing to the byte; 29,156,757 messages. Run on WSL2 Ubuntu on an Intel Core Ultra 7 155H laptop (8 logical cores exposed to Linux), GCC 15.2, `Release`. This is real exchange data, but a *small venue*: see "What this does not show".

**1. Parse + reference book, AAPL, whole file** (`gzip -dc ... | replay_itch50 - --symbol AAPL --no-engine`): **3.6 s wall including decompression, about 8M messages/s end to end.** 0 malformed, 0 truncated frames. 34,509 adds (34,496 `A` + 13 `F`), 1,452 executions (1,436 `E` + 16 `C`), 2 partial cancels, 265 replaces, 33,425 deletes; 0 unknown references, 0 over-reductions, 0 duplicates, 0 crossed states. The book **ends the day empty** -- NASDAQ removes everything by close, so a misread shares field in any of add/execute/cancel/replace would leave orders behind.

**2. Engine vs reference, best bid/ask (price and quantity), at cut points** -- the real pipeline and matching engine fed by the translator, `--max-msgs N`:

| Symbol | Messages read | Msgs to engine | Resting orders | Best bid x qty / ask x qty | Fills | Result |
|---|---|---|---|---|---|---|
| AAPL | 8,000,000 | 9,162 | 17 | 285.59 x 100 / 285.89 x 100 | 0 | engine == reference |
| AAPL | 16,000,000 | 33,802 | 16 | 291.23 x 100 / 291.45 x 15 | 0 | engine == reference |
| AAPL | 24,000,000 | 55,939 | 15 | 291.11 x 100 / 291.56 x 100 | 0 | engine == reference |
| SPY | 8,000,000 | 179,832 | 47 | 320.64 x 100 / 320.68 x 200 | 0 | engine == reference |
| SPY | 16,000,000 | 344,816 | 57 | 321.63 x 103 / 321.65 x 16 | 0 | engine == reference |
| SPY | 24,000,000 | 506,358 | 56 | 321.16 x 131 / 321.18 x 20 | 0 | engine == reference |

**What this does not show.** (a) Nasdaq BX is a small venue -- 15-60 resting orders per symbol, so the engine's 65,536-order capacity (BUGS_FOUND.md #17) was never approached; that needs a main-venue day. (b) This run compared the touch only. The tool now compares every level (price, total quantity, order count); that is verified on synthetic data, including a mutation test (corrupting a few orders by one share left the touch correct in 6/6 runs, so a touch-only check would pass them all; the depth check flagged 5/6) -- but it has **not yet been re-run on the real file**. (c) Partial cancels (`X`) are rare in this file (1-54 per symbol at these cut points), so that path is exercised but thinly. (d) The throughput figure is reference-only and includes decompression; no engine-mode throughput is claimed. (e) One machine, one run per cell, no repeat-run variance measured.

## Isolated-core (fill on real hardware)

```bash
taskset -c 2 chrt -f 50 ./build/bench_pipeline 200000
taskset -c 2 chrt -f 50 ./build/bench_tick_to_trade 100000
```
