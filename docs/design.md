# Design

Preserve component invariants: no syscalls on match path; explicit queue full; logger off hot path.

```
Producer -> submit(MarketDataMsg) -> SPSC inbound
Engine thread -> match -> SPSC outbound
Drain thread -> poll_report -> log sink
```

Conservation requires pure event generation: make_synthetic_stream(seed, n, mid, symbol).

Stop: running_=false; engine.stop() joins; drain exhausts outbound; close sink. Sleep is not synchronization.
