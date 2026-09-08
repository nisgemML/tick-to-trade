# Limitations

| Area | Current | Not claimed |
|------|---------|-------------|
| Feed | Deterministic synthetic stream | Live exchange multicast / co-lo wire |
| Match | Full options-engine core | Multi-venue SOR |
| Log | File sink on drain thread | Default io_uring in portable CI |
| Latency | Software pipeline throughput | NIC HW timestamp to exchange ACK |

- Matching stays single-threaded (options-engine contract)
- Logging never runs inside the match loop
- submit failure under load is visible (queue_full_rejects)
- Unpinned numbers are not wire latency
