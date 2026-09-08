# Benchmark results

## Policy

| Claim type | Allowed when |
|------------|----------------|
| Software pipeline throughput | Any host; must state unpinned vs pinned |
| Software tick-to-trade (recv_ns → fill observed) | Concurrent drain; burst vs steady labeled |
| Component microbench | See third_party/*/BENCHMARK_RESULTS.md |
| Exchange wire / NIC HW timestamp | **Not claimed in this repo** |

## Environment (this recorded run)

```
Host: shared CI/dev container (not isolcpus)
CPU: available cores shared
Compiler: g++ 13, -O2/-O3 Release
Build: cmake -DCMAKE_BUILD_TYPE=Release
Pin: none (engine_cpu=-1)
SCHED_FIFO: no
Date: 2026-09-07
```

## Pipeline throughput (`bench_pipeline`)

| Events | Fills | Throughput | Queue full |
|--------|-------|------------|------------|
| 30000 | 23938 | **~40,041 msg/s** | 0 |

## Software tick-to-trade (`bench_tick_to_trade`)

Definition: `now_at_fill_observed - recv_ns_at_submit` for fills whose order_id was stamped.

| Mode | Events | Samples | p50 | p99 | max | Throughput |
|------|--------|---------|-----|-----|-----|------------|
| steady | 15000 | 13763 | ~37 ms | ~68 ms | ~69 ms | ~23k msg/s |
| burst | 15000 | 13763 | ~30 ms | ~57 ms | ~58 ms | ~27k msg/s |

**Interpretation (important for interviews):**  
On an unpinned shared core with async drain, these percentiles are dominated by **queueing + scheduling**, not by the nanosecond matching kernel. That is expected.  
Isolated-core + `SCHED_FIFO` + paced input is required before quoting sub-microsecond software path numbers.  
The **measurement plumbing is in place** (`submit_with_ts` + histogram). Re-run:

```bash
taskset -c 2 chrt -f 50 ./build/bench_tick_to_trade 100000
taskset -c 2 chrt -f 50 ./build/bench_pipeline 200000
```

## Feed path (`run_feed_pipeline`)

MoldUDP synthetic → GapBuffer → ItchAdapter → MatchingEngine:

| Events | Fills | ttt samples |
|--------|-------|-------------|
| 5000 | 4520 | 4520 |

## Correctness (not performance)

| Test | Result |
|------|--------|
| backpressure | 65535 accepted, 4465 rejected |
| conservation | identical fills/qty across two runs |
| feed_adapter | ITCH Add → MarketDataMsg |
| pipeline_smoke | 2000 events, fills > 0 |

## Component depth (vendored)

- options-engine: differential testing / sanitizer bug history  
- mpsc-queue: formal acq/rel proof + TSan litmus  
- io-uring-queue: failure-mode documentation  
- udp-multicast-receiver: gap/A-B/timestamp path  
