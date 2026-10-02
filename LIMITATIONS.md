# Limitations

| Area | Current | Not claimed |
|------|---------|-------------|
| Feed | Real UDP multicast socket (`feed::MulticastReceiver`, SO_TIMESTAMPING) on a local group, verified end-to-end (`tests/test_live_multicast.cpp`, see BUGS_FOUND.md #13); also supports the original deterministic synthetic stream | A real exchange's actual multicast feed, co-lo wire latency, real packet loss/reordering under production conditions |
| Match | Full options-engine core | Multi-venue SOR |
| Log | Blocking `write()` by default; real `ioq::IOURingLogger` via `-DHFT_WITH_IOURING=ON` (see BUGS_FOUND.md #12) | Default build using io_uring with no opt-in step |
| Latency | Software pipeline throughput | NIC HW timestamp to exchange ACK |

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
