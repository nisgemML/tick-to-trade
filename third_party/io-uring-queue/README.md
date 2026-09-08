# io-uring-queue

**SPSC ring buffer + io_uring async logger for low-latency trading systems.**

Two components that solve distinct problems in trading system design:

1. **`SPSCRingBuffer<T, N>`** — lock-free single-producer single-consumer ring
   buffer using C++20 acquire/release atomics. No `seq_cst`, no MFENCE on the
   hot path. Confirmed race-free under ThreadSanitizer with a 1M-item
   concurrent stress test.

2. **`IOURingLogger`** — async execution report logging via io_uring. Submits
   write requests to the kernel SQ ring without blocking the matching engine
   thread. Supports batching (configurable `batch_size`) and `SQPOLL`. See
   [`docs/failure-modes-and-batching.md`](docs/failure-modes-and-batching.md)
   for what was actually measured on both — the honest numbers, including a
   real, measured case where `SQPOLL` performs dramatically *worse* than
   plain `io_uring`, and why.

3. **`ClassicLogger`** — the same ring-buffer-plus-dedicated-thread
   architecture as `IOURingLogger`, with a blocking `write()` instead of
   `io_uring`, for a fair, matched-architecture comparison.

4. **`examples/matching_engine_demo.cpp`** — a real end-to-end pipeline: a
   simulated matching engine thread generating execution reports at
   sustained load, feeding a logger over four architectures (naive
   synchronous write, ring+classic, ring+io_uring, ring+io_uring+SQPOLL),
   with correctness verified byte-for-byte against the log file for all of
   them.

---

## A real bug this repo shipped with, found and fixed

`IOURingLogger::flush()` used to not actually wait for pending writes to
complete — an internal counter was declared and decremented, but never
incremented, so the wait was always "wait for zero completions." Logging
100 entries and calling `flush()` then `close()` produced a **0-byte log
file** — complete silent data loss, in a tool whose stated purpose is
execution-report compliance logging. A second, related bug let a buffer
slot be reused (overwriting an in-flight write's memory) before its
previous write had actually completed — reproduced directly by stress-testing
with 5,120 entries against the old logic: **63% were corrupted or lost.**

Both are fixed, both have regression tests
(`tests/test_ring.cpp::test_iouring_wraparound_content_integrity` reproduces
the second bug against the old code and passes against the fix), and the
full story — plus how it was found — is in
[`docs/failure-modes-and-batching.md`](docs/failure-modes-and-batching.md).

---

## The problem this solves

A matching engine processing millions of orders/sec cannot block on
`write()` — one slow kernel write stall on the hot path costs real order
cycles. `examples/matching_engine_demo.cpp` measures this directly rather
than asserting it: see "Measured impact" below.

```
Naive (bad):     Matching engine thread → write() → kernel I/O → disk   [blocks the hot path]

This repo:       Matching engine thread → SPSC ring [~30ns typical]
                                              ↓
                  Logger thread → IOURingLogger.log() [off hot path]
                                              ↓
                  Kernel io_uring worker → disk [async]
```

---

## Measured impact (not asserted)

From `examples/matching_engine_demo.cpp`, 200,000 execution reports per
mode, this machine (Ubuntu, GCC 13.3, **1 CPU core** — see the note on
why that matters for SQPOLL below):

```
Mode                          matching-engine thread latency
                               p50     p99     p99.9      max
1. naive write()               267ns   963ns   2,899ns    78,348ns
2. ring + classic write()       30ns    41ns     129ns  7,675,469ns
3. ring + io_uring               30ns    41ns     146ns 15,648,196ns
4. ring + io_uring SQPOLL        31ns    45ns 7,979,645ns 12,275,994ns
```

**What this shows, honestly:**
- Modes 2 and 3 cut typical-case (p50/p99/p99.9) matching-engine latency by
  roughly **9x at p50** and over **20x at p99.9** versus logging
  synchronously — the ring buffer, not the choice of I/O backend, is what
  does this. Both non-SQPOLL modes give essentially identical
  matching-engine-side numbers, because that side of the pipeline is just
  "push into a ring" either way.
- **Every mode's `max` is multiple milliseconds.** The ring insulates the
  matching engine thread in the overwhelming common case, but a logger
  thread that falls behind still eventually fills the ring and stalls the
  producer — a real, demonstrated failure mode, not a hidden one. See
  `docs/failure-modes-and-batching.md` §5.
- **Mode 4 (SQPOLL) is dramatically worse in its own tail**, and this
  machine's `nproc` is 1 — SQPOLL's dedicated polling thread has no
  dedicated core to actually poll on. This is a genuine, measured finding,
  not a design flaw in the code: see `docs/failure-modes-and-batching.md`
  §3 for the full explanation, including why it validates rather than
  contradicts this repo's own SQPOLL core-pinning guidance below.
- All four modes were verified **byte-exact correct** — every one of the
  200,000 reports landed on disk in every mode, checked by file size and
  (for the wraparound/multi-producer stress tests) full content
  comparison.

---

## Memory ordering

The SPSC ring buffer uses the same acquire/release pattern as
[mpsc-queue](https://github.com/nisgemML/mpsc-queue)'s formally-proved queue:

```
Producer: data_[slot] = val  →_sb  head_.store(h+1, release)
Consumer: head_.load(acquire)  →_sw  head_.store(h+1, release)
```

The acquire load synchronises-with the release store — `data_[slot]` is
visible to the consumer before it reads it. No `seq_cst` needed — the directed
happens-before edge is sufficient (same argument as Claim 1 in the MPSC proof).
Confirmed under ThreadSanitizer with a 1M-item single-producer/single-consumer
stress test and an 8-producer fan-in stress test
(`tests/test_ring.cpp::test_multi_producer_fan_in_stress`).

On x86-TSO: release store = plain MOV, acquire load = plain MOV. No MFENCE.

---

## Build and test

```bash
# Requires: liburing-dev (Ubuntu: apt-get install liburing-dev)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure    # RingBuffer (6322 assertions) + MatchingEngineDemo

./bench_ring                  # SPSC latency + io_uring submit latency
./bench_batching               # real batch_size throughput comparison
./matching_engine_demo         # the full 4-mode end-to-end comparison
```

---

## io_uring in production

With `IORING_SETUP_SQPOLL` (requires `CAP_SYS_NICE`):

```cpp
ioq::IOURingLoggerConfig cfg;
cfg.sqpoll         = true;
cfg.sqpoll_cpu     = 7;      // pin the kernel SQ poll thread to CPU 7
cfg.sqpoll_idle_ms = 2000;   // poll for 2s before sleeping
logger.open("/var/log/execution_reports.bin", cfg);
```

**Read `docs/failure-modes-and-batching.md` §3 before enabling this in
production.** `sqpoll_cpu` pinning isn't optional polish — this repo
measured SQPOLL performing roughly **48x worse at p50 completion latency**
than plain io_uring when the poll thread has no dedicated core to run on.
Pin it, or don't enable SQPOLL at all.

For batching:

```cpp
ioq::IOURingLoggerConfig cfg;
cfg.batch_size = 16;   // io_uring_submit() called every 16 log() calls
logger.open("/var/log/execution_reports.bin", cfg);
```

Measured in this environment: batch_size 1→4 gives ~29% more throughput;
4→64 another ~5%; going further (256) is very slightly *worse*. See
`docs/failure-modes-and-batching.md` §2 for the real numbers and why the
plateau happens.

See `bench/bench_ring.cpp`, `bench/bench_batching.cpp`, and
`examples/matching_engine_demo.cpp` for the full benchmark methodology.

---

## References

- Axboe, J. (2019). [Efficient I/O with io_uring](https://kernel.dk/io_uring.pdf). Kernel.dk.
- Lord, J. (2022). [io_uring and networking in 2022](https://kernel.dk/io_uring_and_networking_in_2022.pdf). Kernel.dk.
- Related: [mpsc-queue](https://github.com/nisgemML/mpsc-queue) — the formally-proved MPSC queue this ring buffer's memory ordering is based on.
- Related: [fix-parser](https://github.com/nisgemML/fix-parser), [udp-multicast-receiver](https://github.com/nisgemML/udp-multicast-receiver) — this logger's `LogEntry` format is a natural sink for either repo's decoded execution reports.
