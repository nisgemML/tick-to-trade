# hft-stack

**Integrated low-latency trading stack for top quant firm interviews.**

Composition of three deep components into one measurable, failure-aware pipeline:

| Layer | Component | Location |
|-------|-----------|----------|
| Matching / LOB | SoA engine + verification culture | `third_party/options-engine` |
| Concurrency | Formal MPSC proof + engine SPSC | `third_party/mpsc-queue` |
| Logging | io_uring design + file sink demo | `third_party/io-uring-queue` |
| Market data | MoldUDP64/ITCH gap handling | `third_party/udp-multicast-receiver` |
| Integration | Feed boundary, pipeline, benches, failure tests | `include/hft`, `tests`, `bench` |

```
Feed (synthetic / future UDP) --MarketDataMsg--> MatchingEngine --ExecutionReport--> Log sink
```

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/run_pipeline 50000
./build/bench_pipeline 100000
./build/run_feed_pipeline 10000   # MoldUDP synthetic -> GapBuffer -> match
```

## Why this repo exists

Specialized repos prove **depth**. Interviewers also ask whether you can **compose** systems without losing correctness, back-pressure discipline, or measurement honesty.

1. Real matching engine (not a toy book)
2. Explicit queue-full failure mode
3. Deterministic stream -> conservation across two runs
4. Drain/log off the match thread (hot-path invariant preserved)
5. Honest benchmark policy (software path != wire time)

## Tests

| Test | What it proves |
|------|----------------|
| `pipeline_smoke` | End-to-end events -> fills |
| `backpressure` | Inbound SPSC rejects when full |
| `conservation` | Same stream twice -> identical fill counts/qty |
| `feed_adapter` | ITCH Add -> MarketDataMsg via GapBuffer |

## Interview narrative (60 seconds)

1. Architecture — single-threaded match, SPSC in/out, drain thread logs
2. Depth — options-engine differential testing + mpsc formal proof
3. Composition bugs — see BUGS_FOUND.md
4. Measurement — software pipeline throughput; isolated-core numbers in BENCHMARK_RESULTS.md when pinned
5. Limits — synthetic feed today; UDP/ITCH adapter boundary in include/hft/feed.hpp

## Milestone status

| Milestone | Status |
|-----------|--------|
| 1. Integrate real matching engine + pipeline | Done |
| 2. Failure modes (back-pressure) | Done |
| 3. Deterministic feed + conservation | Done |
| 4. Bench harness + benchmark policy | Done |
| 5. Interview docs (LIMITATIONS, BUGS_FOUND, design) | Done |

## Layout

```
hft-stack/
├── third_party/options-engine/
├── third_party/mpsc-queue/
├── third_party/io-uring-queue/
├── include/hft/{pipeline,feed,log_sink}.hpp
├── tools/run_pipeline.cpp
├── bench/bench_pipeline.cpp
├── tests/
├── BUGS_FOUND.md
├── BENCHMARK_RESULTS.md
├── LIMITATIONS.md
└── docs/design.md
```
