# Limitations

| Area | Current | Not claimed |
|------|---------|-------------|
| Feed | Deterministic synthetic stream | Live exchange multicast / co-lo wire |
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
