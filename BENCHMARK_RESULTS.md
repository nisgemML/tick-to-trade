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

## Isolated-core (fill on real hardware)

```bash
taskset -c 2 chrt -f 50 ./build/bench_pipeline 200000
taskset -c 2 chrt -f 50 ./build/bench_tick_to_trade 100000
```
