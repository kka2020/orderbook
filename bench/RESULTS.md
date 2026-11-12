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
  empty timer pair (~40 TSC ticks; the TSC runs at a fixed rate, measured at ~2.6 GHz, independent of turbo) is subtracted. Each cell below is the median across 11 rounds, with
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

## v1 `BaselineBook` (2026-11-12, with input validation)

`std::map` of price levels → `std::list<Order>` per level, `std::unordered_map` id index. New
orders are validated (zero qty, invalid price, duplicate live id) before matching.

| op | p50 ns | p90 ns | p99 ns | p99.9 ns |
|---|---:|---:|---:|---:|
| new | 55 (53–65) | 93 (87–129) | 158 (147–238) | 303 (265–455) |
| cancel | 78 (75–90) | 108 (101–167) | 184 (157–279) | 371 (268–573) |
| modify | 132 (125–149) | 184 (171–272) | 280 (247–446) | 557 (439–898) |

Throughput: **12.89 M msgs/s** (range 11.12–13.66).

cachegrind, per message: **401 instructions, 5.36 L1d misses, 0.667 last-level data misses**.

This is the baseline every v2 step is compared against. History: before validation was added
(commit d57028e, 2026-11-11) v1 ran 387 instructions per message; the extra 14 come from the
new checks, most likely dominated by the duplicate-id hash lookup. Its latency moved by less than the round-to-round spread.

## v2 `FastBook`, step 0: same containers, new structure (2026-11-12)

Same data structures as v1. New-order handling is split into `validate → ack → match →
after_match`; the per-level fill loop moved into a `FifoPolicy` template parameter (called with a
lambda); the id table is `reserve`d to the capacity hint (65,536) at construction.

cachegrind, per message (v1 in brackets):

| build | instructions | L1d misses | LL data misses |
|---|---:|---:|---:|
| step 0 as committed | 393 (401) | 4.85 (5.36) | 0.706 (0.667) |
| step 0 with the `reserve` line removed | **401** (401) | **5.36** (5.36) | **0.667** (0.667) |

- **The new structure costs nothing**: without the `reserve`, the counts match v1 exactly. The
  policy template and its lambda are inlined away at `-O3`.
- **All of the difference comes from the `reserve`.** With 65,536 buckets for ~5,000 live orders, hash
  chains are shorter (fewer instructions and L1 misses), but the bucket array is larger (512 KB),
  which most likely explains the slightly higher last-level misses. Not yet verified beyond these counts.

Latency, 11 interleaved rounds in the same run (v1 in the same run in brackets; that run was slower
overall than the v1 baseline above, so compare within the run only):

| op | p50 ns | p99 ns |
|---|---:|---:|
| new | 47 (63) | 143 (187) |
| cancel | 82 (88) | 230 (208) |
| modify | 125 (148) | 302 (313) |

Throughput: 13.92 M msgs/s (v1: 11.49 in the same run). The ranges overlap for cancel and modify;
new orders are clearly faster, consistent with the shorter hash chains on the insert path.

## v2 step 1: cached level totals (2026-11-12)

Each price level stores its total resting quantity, updated on rest, fill and cancel, so
`depth_at` reads one number instead of summing the level's queue (O(1) instead of O(orders)).

cachegrind, per message:

| | instructions | L1d misses | LL data misses |
|---|---:|---:|---:|
| v1 | 401 | 5.36 | 0.667 |
| v2 step 0 | 393 | 4.85 | 0.706 |
| v2 step 1 | **400** | 4.87 | 0.706 |

- **Cost: +7 instructions per message** over step 0 (the extra add/subtract on every rest, fill
  and cancel, plus the wider level record). Cache behaviour is unchanged.
- **Benefit: not visible here.** This workload never calls `depth_at`; the gain is for callers that
  query depth (a market-data publisher would after every update). The trade is a small fixed cost
  per update in exchange for a constant-time depth query.

Latency, 11 interleaved rounds (v1 in the same run in brackets):

| op | p50 ns | p99 ns |
|---|---:|---:|
| new | 43 (59) | 118 (165) |
| cancel | 75 (81) | 179 (180) |
| modify | 111 (135) | 237 (280) |

Throughput: 15.43 M msgs/s (v1: 13.18 in the same run). Step 0 and step 1 were measured in
different runs, so their latencies are not compared directly; at ~2% of the instruction count the
step 1 cost is below what this machine's latency can resolve.

For context, v1 also passes the flash1 benchmark on all five scenarios (`flash1/run.sh`), at
about 9–10 M msgs/s on that workload, which differs from this one.
