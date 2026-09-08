# Failure Modes, Batching, and SQPOLL — What Was Actually Measured

This documents three things this repo previously asserted rather than
measured: what happens when things go wrong, whether batching helps, and
whether SQPOLL delivers the near-zero submission cost it's often quoted
at. All numbers below come from an actual run in this environment —
`bench/bench_batching.cpp` and `examples/matching_engine_demo.cpp` — with
the environment's constraints stated plainly where they change the
answer, most importantly: **this machine has exactly 1 CPU core.** That
turns out to matter a great deal for one of these questions.

---

## 1. The correctness bug this document exists because of

Before anything else: `IOURingLogger::flush()` used to not actually wait
for pending writes to complete. `inflight_` was tracked, decremented on
every reaped completion, but **never incremented** anywhere — so
`flush()`'s `io_uring_submit_and_wait(&ring_, inflight_)` was always a
wait for zero completions. Concretely: logging 100 entries, then calling
`flush()` then `close()`, produced a **0-byte file**. Every entry was
silently lost. The repo's own test for this accepted the outcome either
way (`sz == N*40 || sz == 0`, commented "kernel deferred — not an error")
rather than catching it.

There was a second, independent bug in the same area: `log()` gated
reusing a buffer slot on `io_uring_get_sqe()` succeeding — i.e., on there
being room in the *submission* ring — not on whether the *previous*
write that used that slot had actually finished. Those are different
things: the kernel frees an SQ ring slot once it's read the entry, which
can happen well before the write it describes has finished touching the
buffer memory. Submitting more than `kQueueDepth` (256) entries in a
tight loop with no delay reproduced this directly: reverting to the old
logic and stress-testing with 5,120 entries corrupted or lost **63% of
them** (`tests/test_ring.cpp::test_iouring_wraparound_content_integrity`
is the regression test, and it fails against the old code and passes
against the fix).

Both are fixed now: `log()` tracks `submitted_`/`completed_` explicitly
and blocks (applying backpressure to the *logger* thread, not the
matching engine thread — see §5) before reusing a slot whose write hasn't
completed; `flush()` genuinely loops until `completed_ == submitted_`.
Write results are also now checked (`cqe->res`) and counted as errors —
previously a failed write (disk full, I/O error, short write) was
indistinguishable from success anywhere in this class.

---

## 2. Batching — measured, not assumed

`bench/bench_batching.cpp`: 100,000 entries, saturating (no artificial
pacing), one `io_uring_submit()` call per `batch_size` log() calls.

```
batch_size    total_ns    ns/entry    entries/ms
1             65928127      659.3       1516.8
4             47021549      470.2       2126.7
16            45636736      456.4       2191.2
64            44472671      444.7       2248.6
256           45798118      458.0       2183.5
```

**Batching helps, meaningfully, up to a small batch size — then flattens
out, and shows a slight regression at the largest size tested.** Going
from batch_size=1 to 4 is a ~29% throughput improvement; from 4 to 64 is
another ~5%; 256 is very slightly worse than 64. The likely explanation
for the plateau: `kQueueDepth` is 256, so a `batch_size` approaching that
means `log()`'s own backpressure check (`submitted_ - completed_ >=
kQueueDepth`) starts kicking in before a full batch's worth of
submissions has even happened, partially defeating the batching itself.

**Practical takeaway:** a small batch size (4-16 in this environment) captures
nearly all of batching's benefit; there's no reason to reach for a large
one, and doing so can mildly hurt rather than help.

---

## 3. SQPOLL — measured, and the result is a real lesson, not the marketing number

This repo's own `README.md` used to quote "SQPOLL: 5-15ns (SQ store
only, no syscall)" as a *design estimate* — it was never actually run in
that author's environment ("SQPOLL requires CAP_SYS_NICE — not available
in container"). This sandbox has `CAP_SYS_NICE` and runs as root, so
SQPOLL was actually testable — and the honest result contradicts the
quoted number:

```
Mode 3 (plain io_uring):     matching-engine p50=31ns  logger-complete p50=165,083ns
Mode 4 (io_uring + SQPOLL):  matching-engine p50=31ns  logger-complete p50=7,991,006ns
```

(Full histograms in `BENCHMARK_RESULTS.md`.) **SQPOLL was dramatically
worse here, not better** — completion latency roughly 48× higher at p50.

**Why, and why this isn't a code bug:** `nproc` on this machine reports
**1**. SQPOLL's entire value proposition depends on the kernel's SQ-polling
thread having a core to itself — it busy-polls the submission ring
continuously so the application never has to make an `io_uring_enter()`
syscall. On a single-core machine, that polling thread has nothing to
poll *on*: it must time-share the one available core with the matching
engine thread, the logger thread, and everything else in the container,
so it gets scheduled in bursts rather than running continuously. Every
submission that lands while the poll thread is off-core sits unprocessed
until the scheduler gets back to it — which, under contention, was
measured here to sometimes take multiple milliseconds.

This matches — and actually validates — production SQPOLL guidance that
existed in this repo's own README before this document did:

```cpp
params.sq_thread_cpu = 7;    // pin the kernel SQ poll thread to CPU 7
```

Core pinning isn't a nice-to-have for SQPOLL; on this evidence, it's the
difference between SQPOLL being the whole point of using io_uring and
SQPOLL being actively counterproductive. This repo could not validate
the *good* case directly on its own single-core development sandbox —
there was no second core available to pin the poll thread to and confirm
the improvement — and said so plainly rather than leaving the gap
unstated.

**Update: the good case, validated on real 8-core hardware.** Once real
multi-core hardware was available (WSL2, Intel Core Ultra 7 155H, 8
cores — see `BENCHMARK_RESULTS.md`'s "Real multi-core run" for the full
numbers), `examples/matching_engine_demo.cpp` was given a CLI argument to
pin the SQ poll thread to a specific core, and the same 200,000-report
comparison was run both unpinned and pinned:

```
SQPOLL unpinned:        logger-complete mean=132,142ns  p50=113,214ns  p99=294,553ns
SQPOLL pinned (core 7): logger-complete mean=102,492ns  p50= 91,456ns  p99=237,098ns
```

Pinning genuinely helped — roughly 19-22% faster across mean/p50/p99,
consistently, not a fluke in one metric. And pinned SQPOLL beat plain
io_uring (Mode 3, completion mean ~330,000ns on the same machine) by
roughly 3.2x — exactly the outcome the `sq_thread_cpu` guidance above
predicts, now actually demonstrated rather than only argued for. The
1-core sandbox's result above is not superseded by this — both are real,
both came from actually running the code, and together they show the
whole shape of the tradeoff: SQPOLL is a genuine win *conditional on* a
dedicated core, and a genuine loss without one. Neither number alone was
the complete picture.

What this still doesn't validate: true `isolcpus`-level isolation (a
core reserved from the OS scheduler entirely, not just this process's
own affinity request) — WSL2 is a real Linux kernel with real
parallelism, but it runs as a Hyper-V VM on a hybrid P-core/E-core CPU,
so "pinned to core 7" is a request the hypervisor's own scheduler still
mediates. The pinned/unpinned comparison above is a fair, controlled,
same-machine comparison regardless — it just isn't the strongest
possible version of "isolated core" that exists.

---

## 4. Why plain io_uring's completion latency is also much worse than a blocking write() here

A second real, measured, and initially counter-intuitive result from
`examples/matching_engine_demo.cpp`:

```
Mode 2 (ring + classic blocking write()):  logger thread mean =    336ns
Mode 3 (ring + plain io_uring):            logger thread (complete) mean = 738,726ns
```

Submitting via io_uring and later reaping the completion is, in this
measurement, **over 2000x slower on average** than a direct blocking
`write()` call from the same logger thread doing the same work. This is
a known, documented characteristic of io_uring for ordinary buffered
writes (no `O_DIRECT`, no registered files/buffers, no SQPOLL): the
kernel typically hands a buffered write off to an `io-wq` worker thread
rather than completing it inline, because buffered writes weren't
natively asynchronous in the kernel's I/O path for a long time. That
handoff is exactly where the extra latency comes from — an `io-wq`
worker has to be scheduled, whereas a direct `write()` call runs on the
calling thread with no handoff at all.

**This does not mean plain io_uring is a bad choice here** — it means the
benefit io_uring provides is specifically to the *submitting* thread
(which never blocks, regardless of how long the eventual write takes),
not to the *total* latency of getting bytes onto disk. Mode 3's
matching-engine-thread numbers are identical to Mode 2's (both are just
"push into a ring"), which is the metric that actually matters for
protecting the hot path — see §5. The completion-latency number matters
for a different question: how far behind can the logger thread's actual
disk-write progress fall, which bounds how large the ring needs to be to
avoid backpressure reaching the matching engine thread.

---

## 5. Backpressure — a real failure mode, demonstrated, not just described

`examples/matching_engine_demo.cpp`'s Mode 2 and Mode 3 matching-engine
latency distributions both show excellent typical-case numbers (p50 ~30ns,
p99.9 under 200ns) **and** a multi-millisecond `max` (8.5ms for classic,
11.9ms for plain io_uring, 12ms for SQPOLL). The ring buffer's whole job
is to insulate the matching engine thread from I/O latency — and it does,
for the overwhelming majority of pushes. But `try_push` failing because
the ring is full (the logger thread has fallen behind) still means the
matching engine thread spins waiting for space, in this demo's
`while (!ring.try_push(e)) pause();` loop. That spin **is** the matching
engine thread paying for I/O latency indirectly, exactly the failure
mode this design is meant to prevent, showing up in the tail once the
logger thread can't keep the ring drained.

**What this means operationally:** the ring's capacity (4096 in this
demo) is a real, load-bearing capacity-planning number, not an arbitrary
constant. It needs to be sized against the logger thread's *actual*
completion throughput under whatever I/O backend is chosen, not against
the matching engine's production rate alone — and per §4, that
completion throughput can look very different from what raw submission
cost would suggest.

---

## 6. A real bug this repo's own CI caught: -EINTR treated as fatal

This section was added after `IOURingLogger::wait_for_one_completion()`
shipped a real, live bug: any negative return from `io_uring_wait_cqe()`
was treated as an unrecoverable ring failure and immediately returned
`false` — including `-EINTR`, which a blocking syscall can return simply
because an unrelated signal interrupted the wait. That's not a ring
failure; it's normal, expected behavior on any real multi-core Linux
system under load, and the correct response — per standard POSIX
practice for every blocking syscall — is to retry, not give up.

**Why this shipped without being caught locally:** this repo's own
development and testing happened on a single-CPU sandbox (see §3) where
SQPOLL ran slowly enough, and overall scheduling pressure was low enough,
that the race window for a stray `-EINTR` landing mid-wait rarely
mattered in practice. CI, running on real (multi-core) GitHub Actions
hardware, hit it directly: `matching_engine_demo`'s SQPOLL mode
(`Mode 4`) submitted only 199,771 of 200,000 entries —
`log()` itself returned `false` 229 times, each one silently dropping an
execution report, surfacing only as a downstream file-size mismatch
rather than a clear diagnostic.

**The fix:** both `wait_for_one_completion()` and the batched
`io_uring_submit()` call site now retry in a loop while the return value
is `-EINTR`, treating any other negative return as the genuine
unrecoverable failure it actually represents.

**The other fix, alongside it:** the demo itself was part of the problem
— it discarded `log()`'s boolean return value with `(void)ok`, so even
before the EINTR fix, there was no way to tell "229 entries failed to
log" from "everything succeeded and something else corrupted the file"
without re-deriving it from a byte count. The demo now counts and prints
`log()` failures explicitly per mode, so this class of bug is diagnosable
from the first failing run, not just detectable.

**The broader lesson:** a benchmark or stress test that never exercises
real scheduling contention (a single-core sandbox, a quiet CI runner)
can hide bugs that only manifest under it. This is exactly why every
number and every "PASS" in this repo's documentation is qualified with
the environment it came from, rather than presented as a portable
guarantee — and why running the same test suite on a genuinely different
machine (this repo's own CI, in this case) is not redundant with local
testing.

---

## 7. A second real bug this repo's own CI caught: a weak retry on SQ-ring-full

Section 6's -EINTR fix was necessary but not sufficient — CI caught a
second, related gap in the same function, on the very next real run
against actual multi-core hardware. `log()` checks two different
resources before it can submit a write: whether this process's own
buffer slot is free (`submitted_ - completed_ >= kQueueDepth`, checked
first, and handled correctly — it blocks and retries via
`wait_for_one_completion()` until the slot frees up), and separately,
whether `io_uring_get_sqe()` can actually get a submission-queue entry.
That second check used to be handled far more weakly: one non-blocking
drain attempt, one retry, then give up and return `false`.

Those two checks look similar but aren't checking the same thing. The
buffer-slot count is this process's own bookkeeping; the SQ ring's
actual head/tail pointers are a separate structure that, under SQPOLL,
the *kernel's own polling thread* updates on its own schedule. There is
a real window where this process's counters say "there's room" while
the underlying ring, still being drained asynchronously by the kernel,
genuinely doesn't have a free slot yet — and on a single-core sandbox
(this project's own development environment) that window rarely opens
wide enough to matter. On real, multi-core CI hardware, it does:

```
--- Mode 4: SQPOLL ---
logger: 198887 submitted, 198887 completed, 0 errors, 1113 log() call failures
ring+sqpoll  : 7955480 / 8000000 bytes  MISMATCH
```

1113 lost writes, and 8,000,000 − 7,955,480 = 44,520 bytes short —
exactly `1113 × sizeof(LogEntry)` (40 bytes). Not a coincidence or a
rounding artifact: every one of those 1113 `log()` calls that reported
failure corresponds exactly to one missing entry on disk. The failures
were real.

**Fixed** by giving the SQ-ring-full case the same treatment as the
buffer-slot check right above it in the same function: block and retry
via `wait_for_one_completion()` until `io_uring_get_sqe()` succeeds or
the ring is genuinely unrecoverable, instead of giving up after one weak
attempt. The now-redundant `drain_completed_nonblocking()` helper (it
only ever existed to serve the weak retry path) was removed rather than
left as unused dead code.

**Lesson:** when a function checks two different preconditions before
proceeding, "these look like the same kind of check" is not a reason to
give them different robustness — especially when only one of the two
resources being checked is actually this process's own state, and the
other is a kernel-managed structure this process doesn't have full
visibility into moment-to-moment.

---

## 8. Failure modes summary

| Failure | Where | Handling |
|---|---|---|
| SQ ring temporarily full | `log()` | blocks and retries via `wait_for_one_completion()` until a slot frees up or the ring is genuinely unrecoverable — see §7; a single weak non-blocking retry used to give up too early on real multi-core hardware |
| `io_uring_wait_cqe()`/`io_uring_submit()` interrupted by a signal (`-EINTR`) | `log()`, `flush()` | retried automatically — not treated as a ring failure; see §6 |
| Buffer slot still in flight | `log()` | **blocks** the calling (logger) thread until the oldest outstanding write completes — see §1 |
| Write fails (`ENOSPC`, `EIO`, short write) | completion (`cqe->res`) | counted in `error_count()`, exact `-errno` in `last_error()` — was previously silently indistinguishable from success |
| `open()`/`io_uring_queue_init()` fails | `open()` | returns `false`; `sqpoll_setup_errno()` reports the specific errno for an SQPOLL setup failure (e.g. missing `CAP_SYS_NICE`) |
| SQPOLL enabled without a dedicated core | kernel scheduling | not detected or prevented by this code — see §3; a real, measured, severe performance failure mode, not a crash |
| Logger thread falls behind, ring fills | matching engine thread | `try_push` fails; the caller decides (this demo spins, which becomes the exact hot-path stall the whole design exists to avoid — see §5) |
| Multiple producer threads, one shared logger | N/A for `SPSCRingBuffer` itself | see `tests/test_ring.cpp::test_multi_producer_fan_in_stress` — the supported pattern is one SPSC ring **per** producer thread, fanned into one logger thread, not a shared multi-producer ring |
