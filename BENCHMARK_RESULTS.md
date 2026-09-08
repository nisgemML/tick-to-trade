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

## Isolated-core (fill on real hardware)

```bash
taskset -c 2 chrt -f 50 ./build/bench_pipeline 200000
taskset -c 2 chrt -f 50 ./build/bench_tick_to_trade 100000
```
