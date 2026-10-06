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

## Real NASDAQ data: main venue, 2020-01-30

`01302020.NASDAQ_ITCH50.gz` from NASDAQ's public sample server: 5,597,158,940 bytes, matching the
directory listing to the byte, `gzip -t` clean, md5 `baa0a7dfbf4384841a01594cd931e5c0`;
**423,285,709 messages**. Same machine as the BX run (WSL2 Ubuntu, Intel Core Ultra 7 155H),
`Release`. Run with `scripts/real_day_check.sh` and `ENGINE_START_NS=34200000000000`: the engine
starts from a snapshot of the reference book at 09:30:00, because a main-venue pre-open book is
legitimately crossed until the opening cross. The reference book runs from the first message.

**Engine vs reference, FULL book** (every price level's price, total quantity and resting-order
count) at 25/50/75/90% of the file:

| Symbol | Cut | Messages read | Msgs to engine | Levels compared (bid+ask) | Fills | Crossed | Anomalies | Mismatched levels |
|---|---|---|---|---|---|---|---|---|
| AAPL | 25% | 105,821,427 | 522,586 | 3,781+1,166 | 0 | 0 | 0 | 0 |
| AAPL | 50% | 211,642,854 | 1,083,780 | 3,723+1,209 | 0 | 0 | 0 | 0 |
| AAPL | 75% | 317,464,281 | 1,634,225 | 3,722+1,178 | 0 | 0 | 0 | 0 |
| AAPL | 90% | 380,957,138 | 1,930,994 | 3,875+1,224 | 0 | 0 | 0 | 0 |
| MSFT | 25% | 105,821,427 | 541,528 | 2,951+817 | 0 | 0 | 0 | 0 |
| MSFT | 50% | 211,642,854 | 1,040,870 | 2,939+835 | 0 | 0 | 0 | 0 |
| MSFT | 75% | 317,464,281 | 1,518,446 | 2,984+765 | 0 | 0 | 0 | 0 |
| MSFT | 90% | 380,957,138 | 1,732,677 | 3,052+792 | 0 | 0 | 0 | 0 |
| TSLA | 25% | 105,821,427 | 354,780 | 3,438+1,085 | 0 | 0 | 0 | 0 |
| TSLA | 50% | 211,642,854 | 640,375 | 3,479+1,049 | 0 | 0 | 0 | 0 |
| TSLA | 75% | 317,464,281 | 842,972 | 3,160+1,099 | 0 | 0 | 0 | 0 |
| TSLA | 90% | 380,957,138 | 923,651 | 3,192+1,230 | 0 | 0 | 0 | 0 |

**All 12 runs agree: 52,745 price levels compared, 0 mismatched; 12,766,884 messages through the
real matching engine across the runs.** No run reported `CAPACITY`: at every cut point each of
these three books held at most 65,536 resting orders.

**What this does not show.** (a) **`fills=0` everywhere.** The replay rebuilds the book from
NASDAQ's own add/execute/cancel/replace/delete messages, so it validates *book maintenance at
real depth*, around 4,000-5,000 price levels per book, not the engine's crossing logic. Matching
is covered by the 1M-event conservation test against an independent model in options-engine. (b)
Levels are compared on price, total quantity and order count, not FIFO order within a level (as
for BX). (c) Three symbols and four cut points, not every symbol at every message. (d) The
65,536-order capacity was not reached by these books at these cut points, so the overflow path
remains validated only by the synthetic scale test (BUGS_FOUND.md #17). (e) No engine-mode
throughput is claimed: each cut point re-streams the file through gzip and the reference book,
which dominates the wall time.

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

**3. Engine vs reference, FULL book** (`scripts/real_day_check.sh`: every price level's price, total quantity and resting-order count, at 25/50/75/90% of the file's 29,156,757 messages):

| Symbol | Cut | Messages read | Msgs to engine | Levels compared (bid+ask) | Fills | Crossed | Anomalies | Mismatched levels |
|---|---|---|---|---|---|---|---|---|
| AAPL | 25% | 7,289,189 | 6,609 | 10+7 | 0 | 0 | 0 | 0 |
| AAPL | 50% | 14,578,378 | 30,257 | 7+8 | 0 | 0 | 0 | 0 |
| AAPL | 75% | 21,867,567 | 50,885 | 9+5 | 0 | 0 | 0 | 0 |
| AAPL | 90% | 26,241,081 | 62,406 | 10+5 | 0 | 0 | 0 | 0 |
| SPY | 25% | 7,289,189 | 172,692 | 16+13 | 0 | 0 | 0 | 0 |
| SPY | 50% | 14,578,378 | 311,315 | 22+12 | 0 | 0 | 0 | 0 |
| SPY | 75% | 21,867,567 | 462,706 | 21+13 | 0 | 0 | 0 | 0 |
| SPY | 90% | 26,241,081 | 554,639 | 22+13 | 0 | 0 | 0 | 0 |

All 8 runs agree: 193 levels, 1,651,509 messages through the real matching engine across the runs. The full-depth comparison is the one validated by a mutation test (corrupting a few orders by one share left the touch correct in 6/6 synthetic runs; the depth check flagged 5/6).

**What this does not show.** (a) Nasdaq BX is a small venue -- 15-60 resting orders per symbol -- so the engine's 65,536-order capacity (BUGS_FOUND.md #17) was never approached; that needs a main-venue day. (b) Levels are compared on price, total quantity and order count, **not FIFO order within a level**: the engine tracks queue priority, the reference book does not. (c) Partial cancels (`X`) are rare in this file (1-54 per symbol at the first-run cut points), so that path is exercised but thinly. (d) The throughput figure is reference-only and includes decompression; no engine-mode throughput is claimed. (e) One machine, one run per cell, no repeat-run variance measured. (f) The reference book and the translator are both mine, and both follow my reading of the spec; independence comes from the third-party oracle test for field decoding and from the real file itself, where a semantic misreading would show up as unknown references, leftover orders at end of day, or crossed books -- none did.

## Isolated-core (fill on real hardware)

```bash
taskset -c 2 chrt -f 50 ./build/bench_pipeline 200000
taskset -c 2 chrt -f 50 ./build/bench_tick_to_trade 100000
```
