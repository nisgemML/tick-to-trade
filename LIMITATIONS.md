# Limitations

| Area | Current | Not claimed |
|------|---------|-------------|
| Feed | Real UDP multicast socket (`feed::MulticastReceiver`, SO_TIMESTAMPING) on a local group, verified end-to-end (`tests/test_live_multicast.cpp`, see BUGS_FOUND.md #13); also supports the original deterministic synthetic stream | A real exchange's actual multicast feed, co-lo wire latency, real packet loss/reordering under production conditions |
| Real exchange data | `replay_itch50`: spec-accurate NASDAQ ITCH 5.0 parsing, independently verified against a third-party implementation and at 1.5M-message scale | **A run on an actual NASDAQ file** — verified on spec-accurate synthetic data only so far. Also not claimed: the live-multicast and synthetic feed paths still use the portfolio's simplified wire layout, not NASDAQ's (BUGS_FOUND.md #15) |
| Book size | The engine rests at most 65,536 live orders per book (options-engine); replay detects overflow and reports it (exit 4) | A liquid real symbol whose concurrent resting orders exceed that — use `--no-engine` for the reference book |
| Match | Full options-engine core | Multi-venue SOR |
| Log | Blocking `write()` by default; real `ioq::IOURingLogger` via `-DHFT_WITH_IOURING=ON` (see BUGS_FOUND.md #12) | Default build using io_uring with no opt-in step |
| Latency | Software pipeline throughput | NIC HW timestamp to exchange ACK |
| GapBuffer allocation | NUMA-node-bound, explicitly hugepage-backed via `-DHFT_WITH_NUMA=ON` (see BUGS_FOUND.md #14); independently verified against the kernel, not just the allocator's own return code | A measured cross-NUMA-node latency penalty — this sandbox has 1 node, same honest constraint as [cpp26-alloc](https://github.com/nisgemML/cpp26-alloc)'s own NUMA work |

- Matching stays single-threaded (options-engine contract)
- Logging never runs inside the match loop, in either sink configuration
- submit failure under load is visible (queue_full_rejects)
- Unpinned numbers are not wire latency
- The io_uring sink is opt-in, not default, specifically so the zero-extra-dependency
  build keeps working on any Linux box without liburing installed; CI builds and
  tests both configurations (see `.github/workflows/ci.yml`), so "opt-in" doesn't
  mean "untested"
- `tools/run_live_pipeline.cpp` / `tools/send_live_itch.cpp` prove the receive path
  (real socket, real kernel timestamps, real gap buffer) composes correctly — on a
  local multicast group (default `239.1.1.1` on `lo`), not a real exchange's network.
  Zero packet loss observed up to a 5,000-packet zero-delay burst in this environment;
  that is a statement about this specific test, not a latency or loss guarantee under
  real network conditions
- The NUMA-aware GapBuffer allocator is opt-in (`HFT_WITH_NUMA`), same reasoning as
  `HFT_WITH_IOURING`; CI builds and runs the real live two-process multicast path under
  it (`integration-numa`, `integration-numa-tsan`), not just a unit test in isolation
- `bench/bench_gapbuffer_numa.cpp` measures whether hugepage backing actually changes
  anything under realistic gap-recovery load (see BENCHMARK_RESULTS.md) — this sandbox's
  honest result is reported there, not assumed from the standalone cpp26-alloc number
