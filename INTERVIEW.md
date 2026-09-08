# Interview one-pager

## Pitch (45–60s)

I built **hft-stack** as a composition layer over four components I also own:

1. **options-engine** — SoA matching + verification culture  
2. **mpsc-queue** — formal memory-ordering proof  
3. **io-uring-queue** — async logger design + failure modes  
4. **udp-multicast-receiver** — MoldUDP64/ITCH gap handling  

The integrated path is:

`MoldUDP/ITCH → GapBuffer → MarketDataMsg → MatchingEngine → ExecutionReport → log`

with explicit **back-pressure**, **conservation tests**, and **tick-to-trade measurement** (`recv_ns` → fill observed).

## What to open live

1. `README.md` — architecture  
2. `BUGS_FOUND.md` — GapBuffer 6MB stack segfault; stop/join drain order  
3. `tests/test_conservation.cpp` — determinism  
4. `tests/test_backpressure.cpp` — queue full is not silent  
5. `bench/bench_tick_to_trade.cpp` — measurement definition  

## Honest limits (say this yourself)

- Default numbers are **software path on unpinned cores**, not co-lo wire time  
- Logger in the default binary is file sink on the drain thread (io_uring component is vendored)  
- Feed demo uses synthetic MoldUDP packets; PCAP/live sockets are in the UDP component  

## Why this is a 9.5-class portfolio piece

- Real components, not toys  
- Failure modes tested  
- Measurement methodology explicit  
- Composition bugs written down  
- Does not over-claim latency  
