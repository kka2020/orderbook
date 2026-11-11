# Benchmark results

How to reproduce:

```
cmake --preset release && cmake --build --preset release
taskset -c 2 build/release/bench/lob_latency 11      # latency + throughput, 11 rounds
bench/cachegrind.sh                                   # instructions + simulated cache misses
```

## Method

- **Workload** (`bench/workload.h`, seed 42): 5,000 resting orders built up untimed, then 1,000,000
  timed messages: 49.2% new, 45.8% cancel, 5.0% modify. Prices cluster within a few ticks of the
  touch and the mid drifts. 3% of new orders are aggressive and 3% are IOC. Of the 481,807 orders
  that rested, **95.1% ended cancelled**, 3.9% fully filled and 1.0% were still resting, close to
  real equity feeds. Every cancel and modify targets a live order (the generator tracks fills
  through a shadow v1 book), so there are no stale rejects.
- **Latency**: each message is timed with `lfence; rdtsc` ... `rdtscp; lfence`. The median cost of an
  empty timer pair (~38 TSC ticks; the TSC runs at a fixed ~2.67 GHz, independent of turbo) is subtracted. Each cell below is the median across 11 rounds, with
  the min–max across rounds in brackets. Every round uses a fresh book. Sample buffers are
  written once before timing, so no page faults occur during the timed loop.
- **Throughput**: a separate replay per round under a single timer (no per-message timers).
- **cachegrind**: 200,000 timed messages, counted only inside the replay (generation and prefill
  are excluded). Its counts come from a simulated cache, so they are identical on every run.
- **Listener**: counts events only, so there is no I/O or allocation inside the timed region.

## Machine and caveats

i7-12700H laptop, WSL2 (Ubuntu 24.04), g++-14 `-O3 -march=native`, pinned with `taskset -c 2`.
WSL2 is a virtual machine: the pin is to a *virtual* CPU, which Windows can move between P-cores
and E-cores, and turbo/thermal state varies. That is the reason for the wide min–max ranges. Treat
the absolute numbers as indicative; the meaningful comparisons are relative (v2 vs v1, same
workload, interleaved rounds) and the deterministic cachegrind counts.

## v1 `BaselineBook` (2026-11-11)

`std::map` of price levels → `std::list<Order>` per level, `std::unordered_map` id index.

| op | p50 ns | p90 ns | p99 ns | p99.9 ns |
|---|---:|---:|---:|---:|
| new | 54 (51–61) | 85 (79–116) | 153 (137–197) | 307 (293–400) |
| cancel | 77 (73–87) | 108 (101–156) | 192 (171–239) | 367 (316–455) |
| modify | 130 (125–147) | 185 (173–254) | 305 (268–389) | 566 (473–713) |

Throughput: **13.16 M msgs/s** (range 11.34–13.84).

cachegrind, per message: **387 instructions, 5.35 L1d misses, 0.667 last-level data misses**.

For context, v1 also passes the flash1 benchmark on all five scenarios (`flash1/run.sh`), at
about 9–10 M msgs/s on that workload, which differs from this one.
