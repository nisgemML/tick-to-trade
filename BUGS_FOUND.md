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

- `third_party/options-engine/README.md` — 12 bugs found via differential testing / sanitizers
- `third_party/io-uring-queue/README.md` — silent data-loss in flush / buffer reuse
- `third_party/mpsc-queue/proof/` — memory-ordering claims under TSan

## Still open (roadmap)

- True `recv_ns` from SO_TIMESTAMPING once UDP feed is adapted
- io_uring sink behind CMake flag without forcing liburing on all CI hosts
- GapBuffer (~6MB) must be heap-allocated; stack local caused SIGSEGV in test_feed_adapter.

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
