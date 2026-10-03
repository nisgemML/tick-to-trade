# Bugs found during integration

Integration is where composition bugs appear. Record them here the same way
options-engine and io-uring-queue document component bugs.

## Integration session

### 1. Queue full is silent unless counted
**Symptom:** `MatchingEngine::submit` returns false under burst; easy to ignore.  
**Fix:** Pipeline increments `queue_full_rejects`; `test_backpressure` asserts rejects > 0 when engine is not started and >65536 messages are pushed.  
**Lesson:** Back-pressure must be a first-class metric, not a bool you discard.

### 2. Draining after `stop()` races if join order is wrong
**Symptom:** Losing tail execution reports if the drain thread exits before outbound is empty.  
**Fix:** Drain loop continues `poll_report` after `running_=false` until empty; `engine.stop()` joined before final stats snapshot.  
**Lesson:** `stop()` join on the engine thread is the happens-before edge — not sleep.

### 3. Logger on the match thread would violate OE hot-path contract
**Symptom:** Design pressure to log inside match callback.  
**Fix:** Dedicated drain thread owns all log I/O; match path only pushes `ExecutionReport`.  
**Lesson:** Preserve component invariants at the composition boundary.

### 4. Non-deterministic synthetic stream breaks conservation tests
**Symptom:** Random prices → two runs differ in fill counts.  
**Fix:** `make_synthetic_stream` is pure function of `(n, seed, mid, symbol)`.  
**Lesson:** Determinism is a prerequisite for pipeline-level conservation.

## Component bugs (already fixed upstream)

See:

- `third_party/options-engine/README.md` — 15 bugs found via differential testing / sanitizers / real multi-core hardware
- `third_party/io-uring-queue/README.md` — silent data-loss in flush / buffer reuse
- `third_party/mpsc-queue/proof/` — memory-ordering claims under TSan

## Still open (roadmap)

- True `recv_ns` from SO_TIMESTAMPING once UDP feed is adapted
- GapBuffer (~6MB) must be heap-allocated; stack local caused SIGSEGV in test_feed_adapter.
- `MarketMaker` (added this session — see `include/hft/market_maker.hpp`,
  `docs/strategy-notes.md`) demonstrates correct inventory/P&L mechanics
  against a real, tested implementation — it is not a calibrated strategy.
  No fitted \(\sigma, k, \gamma\), no adverse-selection model, no backtest
  harness. Those remain open, deliberately, as a separate and larger piece
  of work — see strategy-notes.md's "What this stack does not claim."

### 5. assert() compiled out under Release (-DNDEBUG)
**Symptom:** All tests used `assert()`. Default `CMAKE_BUILD_TYPE=Release` defines `NDEBUG`, so every CHECK was a no-op; ctest always "Passed" even with impossible conditions.  
**Fix:** `hft/check.hpp` CHECK/TEST_EXIT macros that always evaluate; CI Release job plus a deliberate-fail binary that must exit non-zero.  
**Lesson:** Composition-layer tests must not rely on assert() if the documented build is Release.

### 6. bench_pipeline timed a fixed 200ms sleep
**Symptom:** `run_stream` slept 40×5ms inside the timed region; reported throughput was dominated by sleep (~10× low).  
**Fix:** `submit_stream` + `wait_until_drained` (poll); bench reports submit-only and e2e separately.

### 7. feed path never exercised GapBuffer gaps
**Symptom:** sequential seq only; gap/duplicate paths untested in integration.  
**Fix:** `test_gap_injection` skips seq2, asserts on_gap, then fills gap and checks delivery order + duplicate drop.

### 8. Pipeline `running_` was a plain bool across threads
**Symptom:** `start`/`stop` wrote `running_` from the control thread while `drain_loop` read it on the drain thread — data race under the C++ memory model; TSan-visible and undefined behavior.  
**Fix:** `std::atomic<bool> running_` with acquire loads / release stores so stop's store synchronizes with the drain loop's load before join.  
**Lesson:** Any flag that crosses threads is an atomic (or mutex), even if "it usually works" on x86.

### 9. `recv_by_order_` — an unordered_map raced across threads, and leaked
**Symptom:** `submit_with_ts()` (control thread) writes `recv_by_order_[order_id]`; `on_fill()` (drain thread) reads/looks it up — a full STL container mutated and read from two threads with zero synchronization, worse than bug #8 (this one includes internal rehashing, not a single scalar). Invisible to every registered ctest: `Pipeline::submit()` always calls `submit_with_ts(msg, 0)`, and the write is gated on `recv_ns != 0`, so no test ever touched the racy code path — only `run_feed_pipeline` and `bench_tick_to_trade` (neither wired into ctest) pass a real timestamp. Confirmed directly under TSan on both tools. Also never erased anything, so the map grew unbounded for the life of the process — harmless for a short benchmark, a genuine leak for any long-running use.  
**Fix:** `std::mutex recv_mu_` guarding both the insert and the find/erase; entries erased on first use (trade-off: a partially-filled order's *later* fills no longer get a tick-to-trade sample, only its first — the more common definition of the metric anyway, and the honest cost of not leaking).  
**Fix, CI:** `run_feed_pipeline` and `bench_tick_to_trade` added directly to the ASan/TSan CI jobs (not just ctest), so this class of bug can't hide behind "the registered tests don't happen to exercise the racy path" again.  
**Lesson:** A side-table that isn't part of the SPSC-protected hot path is easy to forget needs its own protection — "the queues are lock-free and correct" doesn't extend automatically to bookkeeping built on top of them.

### 10. `run_market_maker_demo` read `OrderBook::best_quote()` from the wrong thread
**Symptom:** The demo's quote loop called `mkt_engine.book_for(symbol)->best_quote()` directly from its own thread while the engine's matching thread was still running — exactly the hazard `MatchingEngine::book_for()`'s own doc comment already warned about (added earlier, in options-engine itself, after `examples/recovery_demo.cpp` hit the same class of bug there). `docs/strategy-notes.md` even already said "book state: available via matching engine after stop (not concurrent-safe while running)" — this file was written before the doc that already contained the answer was checked. TSan caught it on the very first run of the tool under a sanitizer; no ctest entry touches this code path at all.  
**Fix:** Derive the quote's reference price only from `last_trade`, updated exclusively through the engine's own thread-safe `ExecutionReport`/SPSC channel — never touch `OrderBook` memory from a second thread while the engine is live.  
**Fix, CI:** `run_market_maker_demo` added to both the ASan and TSan "exercise non-ctest tools" steps.  
**Lesson:** A documented constraint in a doc file doesn't enforce itself — new code has to actually be checked against it, not just assumed to comply because it was written after the doc existed.

### 11. Vendored io-uring-queue's SQPOLL path lost writes on real multi-core CI
**Symptom:** `third_party/io-uring-queue`'s `IOURingLogger::log()` checks two different resources before submitting a write: whether this process's own buffer slot is free (handled correctly, blocks and retries), and separately whether `io_uring_get_sqe()` can get a submission-queue entry from the actual ring — that second check only got a single non-blocking drain attempt, then gave up. Every local dev/test run (single-core sandbox) passed clean. The first real run on GitHub's own multi-core CI runner — testing this exact vendored snapshot, composed into this exact repo — reported `1113 log() call failures` in SQPOLL mode, and the resulting file was short by exactly `1113 * sizeof(LogEntry)` bytes: not a flaky test, a real, reproducible gap that only real multi-core hardware exposed. Root cause: under SQPOLL, the kernel's own polling thread updates the SQ ring's actual state asynchronously — there's a real window where this process's own bookkeeping says "there's room" while the ring, still being drained by the kernel on its own schedule, genuinely isn't caught up yet.  
**Fix:** Gave the SQ-ring-full case the same blocking-retry treatment as the buffer-slot check right above it in the same function — keep waiting via the existing completion-wait helper until `io_uring_get_sqe()` succeeds or the ring is genuinely unrecoverable, instead of giving up after one weak attempt. Full detail in `third_party/io-uring-queue/docs/failure-modes-and-batching.md` §7.  
**Lesson:** vendoring a component doesn't mean its every code path has been exercised under every condition it will actually run under — a real CI run on genuinely different (multi-core) hardware than this repo's own development environment found a bug the vendored source's own prior single-core testing never could.

### 12. The README advertised io_uring logging; the pipeline only ever built the blocking path
**Symptom:** `include/hft/log_sink.hpp`'s own comment promised a `CMake option HFT_WITH_IOURING` to swap in `ioq::IOURingLogger`; that option did not exist anywhere in `CMakeLists.txt`. `grep -rn IOURingLogger` across the whole repo matched only that one comment — the vendored `third_party/io-uring-queue` (including the real, hard-won SQPOLL fix in bug #11 above) was never linked into this pipeline at all. The README's feature table listed "io_uring async logger" as if it were the active path; `LIMITATIONS.md` was more honest ("File sink on drain thread") but the inconsistency between the two was never resolved — this is exactly the "advertised vs. actually wired" gap a technical reviewer would find by grepping the source, not reading the prose.
**Fix:** Added the actual `HFT_WITH_IOURING` CMake option (`OFF` by default, so the zero-extra-dependency build keeps working everywhere); `IOURingLogSink` in `log_sink.hpp` wraps the real `ioq::IOURingLogger` behind the same four-method surface as `FileLogSink`, selected via a `LogSink` type alias so `Pipeline`'s own code needs no branching. `hft::LogEntry` and `ioq::LogEntry` are independently-defined but byte-identical 40-byte layouts; `convert_entry()` does an explicit field copy rather than a `reinterpret_cast`, so a future layout change in either struct fails to compile instead of silently aliasing.
**Verified, not just compiled:** ran `run_feed_pipeline` under both configurations with identical input and diffed the two 7,236-record output logs field-by-field, excluding `timestamp_ns` (which cannot match between two separate process runs by construction) — zero mismatches across every other field. Both configurations also pass the full test suite and run clean under TSan, including the specific tools (`run_feed_pipeline`, `bench_tick_to_trade`, `run_market_maker_demo`) that caught bugs #8-#10 above.
**Lesson:** a vendored dependency with its own real bug-fix history (bug #11) is still dead weight if nothing in the composing repo actually calls it — "we found and fixed a real bug in X" and "X is wired into this pipeline" are two separate claims, and only one of them was true here until now.

### 13. The first draft of the live-multicast tool repeated an already-documented bug, in the same file that documents it
**Symptom:** Writing `tools/run_live_pipeline.cpp` — the live `feed::MulticastReceiver` wiring this entry's own roadmap had listed as open — the first draft declared `feed::GapBuffer gap_buf(gcfg);` as a stack local in `main()`. Immediate segfault, zero output, not even the first `printf`, exit code 139. This is the *exact* ~6MB-stack-local issue already on record two paragraphs above this one in this same file's "Still open (roadmap)" section at the time, from `test_feed_adapter`'s own history — documenting a pitfall didn't stop it from almost happening again in new code written by someone who had, in principle, already read the file.
**Fix:** `auto gap_buf = std::make_unique<feed::GapBuffer>();`, matching the pattern `tools/run_feed_pipeline.cpp` already used correctly. One-line fix once caught; the point of this entry is that it needed catching at all.
**Verified, not just fixed:** ran the real end-to-end path — a separate `tools/send_live_itch.cpp` process emitting real MoldUDP64/ITCH packets over an actual UDP multicast socket (`239.1.1.1:15001` on `lo`, `SO_TIMESTAMPING` enabled) into `run_live_pipeline` — 5 repeated runs, byte-identical results every time (2,000/2,000 packets received and delivered, 0 parse errors, 1,591 fills), then a 5,000-packet burst with zero send delay to check for drops (none), then the same path again under ThreadSanitizer (clean). A new, single-process automated version (`tests/test_live_multicast.cpp`) now runs this for real on every CI push across Release/ASan/TSan, rather than relying on the two standalone tools being run by hand.
**Lesson:** a documented bug in a bugs-found file is a record, not a guardrail — it doesn't prevent the same mistake in a different file, written later, by someone (human or not) who hasn't specifically cross-checked new code against it. The actual guardrail is the test that now runs on every push, not the paragraph that explains what went wrong last time.

### 14. A real correctness bug in a different repo, found only by actually using it for something real
**Symptom:** Wiring [cpp26-alloc](https://github.com/nisgemML/cpp26-alloc)'s `alloc::numa::alloc_on_node` (NUMA-aware, explicitly hugepage-backed allocation) in to back `GapBuffer`'s real ~6MB backing storage — `-DHFT_WITH_NUMA=ON`, see `include/hft/numa_support.hpp` and `CMakeLists.txt` — the allocation failed outright with `BindFailed`, every time, on a structure that fit comfortably within the reserved hugepage budget.
**Root cause, found in the other repo, not this one:** `mbind()` requires its length argument aligned to the *governing* page size of the mapping — the real hugepage size (2MB on this hardware, read from `/proc/meminfo`, not hardcoded), not the regular 4KB page size, for a `MAP_HUGETLB` region. `cpp26-alloc`'s `alloc_on_node()` passed the raw, unaligned requested size straight through to `mbind()`. Its own test (`test_numa.cpp`) never caught this because the test region was exactly 2MB — accidentally already aligned. `GapBuffer` at ~5.97MB is not a multiple of 2MB, which is the ordinary case for any real structure, not an edge case.
**Fix (in `cpp26-alloc`, re-vendored here afterward):** round the `mbind()` length up to a multiple of the real hugepage size, queried from `/proc/meminfo` rather than assumed, only on the hugepage-backed path (the regular-page fallback needs no such alignment). Added a regression test there using a deliberately non-2MB-aligned size, so this specific failure mode can't silently return.
**Verified, not just fixed:** the fixed allocator correctly reports `hugepage_backed=1` for the real `GapBuffer` allocation, confirmed stable across 3 repeated full two-process live-multicast runs (`run_live_pipeline` + `send_live_itch`, 2,000/2,000 packets, 1,591 fills every time, identical to the non-NUMA path's results) and clean under TSan. CI (`integration-numa`, `integration-numa-tsan`) now exercises this exact path — real hugepages reserved, real `HFT_WITH_NUMA=ON` build, real live two-process multicast run — on every push.
**Lesson:** this is exactly the kind of bug a standalone unit test can miss by coincidence — a test input that happens to already satisfy an alignment constraint the code never actually enforces, passing not because the code is correct but because the test never exercised the constraint. It took a second, independent, real use of the same code (a genuinely differently-shaped allocation, in a different repo, for a different purpose) to find it. That's also the argument for actually wiring NUMA-aware allocation into something real instead of leaving it as a standalone utility nobody calls with a real workload — the bug was there in `cpp26-alloc` the whole time; nothing found it until something needed it for real.
