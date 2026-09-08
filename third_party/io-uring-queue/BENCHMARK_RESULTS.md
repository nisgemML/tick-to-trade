# Benchmark Results — io-uring-queue

Every number here comes from an actual run in this environment — nothing
is asserted without a run backing it, and where the original version of
this repo asserted a number as a design estimate (SQPOLL's "~5-15ns"),
that's now replaced with what was actually measured, including where the
measured result contradicts the estimate.

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure       # RingBuffer (6322 assertions) + MatchingEngineDemo
./bench_ring                     # SPSC latency + io_uring submit latency
./bench_batching                  # batch_size throughput comparison
./matching_engine_demo            # 4-mode end-to-end comparison
```

**Environment:** Ubuntu 24.04, GCC 13.3, Linux 6.18, x86-64 container,
**1 CPU core** (`nproc` reports 1) — stated explicitly because it directly
explains the SQPOLL result below; this is not a detail to skim past.
Also verified clean under AddressSanitizer, UndefinedBehaviorSanitizer,
and ThreadSanitizer (`-fsanitize=address,undefined` / `-fsanitize=thread`).

---

## A note on measurement methodology, up front

Three separate histogram implementations in this repo (this file's
predecessor, `bench/bench_ring.cpp`'s original `Hist`, and this update's
first version of `examples/matching_engine_demo.cpp`'s `Hist`) used a
fixed-size bucket array and silently folded any value past the array's
range into the last bucket. The tell: `bench_ring.cpp`'s old io_uring
numbers showed **p99 and p99.9 identically equal to 4095** — the array's
exact upper bound. That's not a coincidence, it's the signature of
clipping. All histograms in this repo now store raw samples and sort them
for percentiles — slower to compute, but incapable of silently lying
about range. The real p99.9 for that same measurement turned out to be
**790,495ns**, not 4095 — a ~193x difference from what the clipped
histogram reported.

---

## Test results

```
2/2 CTest suites passed

  RingBuffer          : 6322 assertions, 0 failed (~8.6s, includes a
                         1M-item concurrent SPSC stress test and an
                         8-producer, 160,000-entry fan-in stress test)
  MatchingEngineDemo   : PASS — 200,000 execution reports x 4 architectures,
                         verified byte-exact correct in every mode (~5.5s)
```

### A real bug this test suite found

`IOURingLogger::flush()` never actually waited for completions —
`inflight_` was declared and decremented but never incremented, so the
wait was always "wait for zero." Logging 100 entries then `flush()` +
`close()` produced a **0-byte file**. A second bug let a buffer slot be
reused before its previous write completed; stress-testing 5,120 entries
against the pre-fix logic corrupted or lost **63%** of them. Both fixed;
both regression-tested (`test_iouring_log_entries`,
`test_iouring_wraparound_content_integrity`). Full writeup:
`docs/failure-modes-and-batching.md` §1.

---

## SPSC Ring Buffer latency

**Single-thread push+pop (2M iterations), real (unclipped) histogram:**

```
push: p50=215ns   p99=258ns   p99.9=528ns
pop:  p50=215ns   p99=258ns   p99.9=538ns
```

These are notably higher than this repo's originally-claimed "p50=12ns" —
that number was never reproduced by an actual run in this environment.
This machine's absolute latency floor is higher across the board (a
single-core, shared/virtualized container, not an isolated bare-metal
core), which is exactly why every number in this document is labeled with
its environment rather than presented as a portable constant. The
**relative** behavior — push and pop cost dominated by a cache hit and a
plain MOV, no syscall, no lock — is what's actually being claimed by the
design, and that holds regardless of the absolute floor.

---

## io_uring async write latency

**100K `LogEntry` writes (40 bytes each) via `io_uring_submit`, real
(unclipped) histogram:**

```
io_uring submit: p50=3,348ns   p99=21,088ns   p99.9=790,495ns
```

The p99.9 in particular is dramatically different from what this
document's clipped-histogram predecessor reported (4,095ns) — see the
methodology note above.

---

## Batching — measured effect

`bench/bench_batching.cpp`, 100,000 entries per `batch_size`, saturating:

```
batch_size    total_ns    ns/entry    entries/ms
1             71082958      710.8       1406.8
4             48381853      483.8       2066.9
16            45952739      459.5       2176.1
64            46829773      468.3       2135.4
256           44493344      444.9       2247.5
```

batch_size 1→4 is a ~32% throughput improvement; returns flatten out past
that, with 256 essentially tied with 64. See
`docs/failure-modes-and-batching.md` §2 for the likely explanation
(interaction with the 256-entry queue depth).

---

## SQPOLL — measured, and the result matters more than the number

```
Plain io_uring (Mode 3):   logger-thread completion p50 = 154,341ns
SQPOLL        (Mode 4):    logger-thread completion p50 = 7,988,146ns
```

**SQPOLL was ~52x worse at p50 in this environment.** This machine has
exactly 1 CPU core — SQPOLL's dedicated polling kernel thread has no core
to itself and must time-share with the application threads, which
defeats its entire purpose (continuous polling without a wakeup). This is
a genuine, measured, explainable result, not a code defect — see
`docs/failure-modes-and-batching.md` §3 for the full explanation and why
it actually validates this repo's own `sq_thread_cpu` pinning guidance,
which this environment cannot itself validate (there's no second core to
pin the poll thread to and show the improvement — an honest gap, stated
as one, not hidden).

---

## End-to-end: matching engine → logger, four architectures

`examples/matching_engine_demo.cpp`, 200,000 execution reports per mode,
this machine, real (unclipped, raw-sample) histograms:

```
Mode 1: naive synchronous write() (no ring, no 2nd thread)
  matching-engine thread   n=200000  mean=290ns   p50=267ns  p99=963ns    p99.9=2899ns    max=78348ns

Mode 2: SPSC ring + dedicated thread + blocking write()
  matching-engine thread   n=200000  mean=1856ns  p50=30ns   p99=41ns     p99.9=129ns     max=7675469ns
  logger thread (write())  n=200000  mean=321ns   p50=285ns  p99=1560ns   p99.9=3491ns    max=29945ns
  200000 written, 0 errors

Mode 3: SPSC ring + dedicated thread + io_uring
  matching-engine thread   n=200000  mean=2659ns  p50=30ns   p99=41ns     p99.9=146ns     max=15648196ns
  logger thread (submit)   n=200000  mean=1108ns  p50=296ns  p99=1744ns   p99.9=66088ns   max=4676149ns
  logger thread (complete) n=200000  mean=703680ns p50=154341ns p99=9931682ns p99.9=12742879ns max=13626749ns
  200000 submitted, 200000 completed, 0 errors

Mode 4: SPSC ring + dedicated thread + io_uring SQPOLL
  matching-engine thread   n=200000  mean=21535ns p50=31ns   p99=45ns     p99.9=7979645ns max=12275994ns
  logger thread (submit)   n=200000  mean=21618ns p50=109ns  p99=2950ns   p99.9=7963614ns max=11969646ns
  logger thread (complete) n=200000  mean=5545017ns p50=7988146ns p99=8296062ns p99.9=12004955ns max=12005847ns
  200000 submitted, 200000 completed, 0 errors

Correctness: all 4 modes logged exactly 8,000,000/8,000,000 bytes — every
report reached disk in every mode.
```

**Reading this honestly:**

- **The matching engine thread is the metric that matters**, and modes 2
  and 3 both cut its p50 by ~9x and p99.9 by over 20x versus naive
  synchronous logging — that's the actual, measured value of the ring
  buffer. The choice between a classic logger and io_uring makes
  essentially no difference to the matching engine thread's own numbers,
  because both are just "push into a ring" from its perspective.
- **io_uring's completion latency is far worse than a classic blocking
  write() in this environment** (mean 703,680ns vs 321ns) — a real,
  documented characteristic of ordinary buffered io_uring writes without
  `O_DIRECT`/registered buffers/SQPOLL (the kernel hands them to an
  `io-wq` worker thread rather than completing inline). This does not
  make io_uring the wrong choice — it means io_uring's benefit is
  specifically "the submitting thread never blocks," not "the write
  finishes faster." See `docs/failure-modes-and-batching.md` §4.
- **Every mode has a multi-millisecond `max`.** The ring buffer bounds
  typical-case latency beautifully but does not eliminate tail risk: if
  the logger thread falls behind for any reason, the ring eventually
  fills and the matching engine thread's `try_push` spins waiting for
  space — exactly the failure mode the whole design exists to avoid,
  showing up when the logger can't keep pace. See
  `docs/failure-modes-and-batching.md` §5.

---

## What changed from the original version of this document

- SQPOLL's "~5-15ns" was a documented design estimate, never run in that
  author's environment. It's now replaced with an actual measurement
  (which is much worse here, for a specific, explained, environmental
  reason — see above).
- The "write() typical: 500-5,000ns" comparison was asserted from general
  knowledge, not measured against this repo's own workload. It's now a
  real `ClassicLogger` implementation, benchmarked in the same harness as
  `IOURingLogger`, under the same load.
- Every histogram in this repo was silently clipping its tail (see the
  methodology note above) — fixed everywhere, and every affected number
  in this file is the corrected, real value.
- `flush()`'s completion-tracking bug (100% data loss under its own
  existing test, before the fix) is documented and fixed, with a
  regression test that reproduces the corruption against the old logic.
