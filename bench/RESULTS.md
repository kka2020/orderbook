# Benchmark results

How to reproduce:

```
cmake --preset release && cmake --build --preset release
taskset -c 2 build/release/bench/lob_latency 11      # latency + throughput, 11 rounds
bench/cachegrind.sh                                   # instructions + simulated cache misses
build/release/bench/lob_allocs                        # heap allocations per message
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
- **Allocations** (`lob_allocs`): calls to `operator new` per message during the timed replay
  (prefill excluded), from a separate binary that replaces `operator new`, so the counter never
  touches the latency or cachegrind runs. Deterministic.

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

Heap allocations per message: new 1.903, cancel 0, modify 2.001, **1.036 overall** (one
`std::list` node and one `std::unordered_map` node per resting order, plus a `std::map` node when
a new price level appears).

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

Heap allocations per message: 1.036, identical to v1 (same containers; the reserve only
pre-sizes the bucket array, which was already large enough).

Throughput: 15.43 M msgs/s (v1: 13.18 in the same run). Step 0 and step 1 were measured in
different runs, so their latencies are not compared directly; at ~2% of the instruction count the
step 1 cost is below what this machine's latency can resolve.

## v2 step 2: order pool + free list (2026-11-12)

Orders move out of the `std::list` nodes into one `std::vector<Order>` pool, reused through a
LIFO free list of 32-bit slot numbers. Each level is still a `std::list`, now of slot numbers; a
small view (`PooledQueue`) presents it to `FifoPolicy` as a queue of orders.

| | instructions | L1d misses | LL data misses | allocations |
|---|---:|---:|---:|---:|
| v1 | 401 | 5.36 | 0.667 | 1.036 |
| v2 step 1 | 400 | 4.87 | 0.706 | 1.036 |
| v2 step 2 | **411** | **4.93** | 0.706 | **1.036** |

- **Allocations unchanged, as predicted**: every resting order still costs a list node and a
  hash node. The pool only grows (rarely) past its reserved capacity.
- **+11 instructions and slightly more L1 misses**: reaching an order now takes an extra hop (list
  node → slot number → pool slot), and cancel/modify read the order through it.
- This step on its own is a small regression. It is the setup for step 3, which puts the queue
  links inside the pooled orders and removes the list (and its allocation) entirely.

Latency, 11 interleaved rounds (v1 in the same run in brackets):

| op | p50 ns | p99 ns |
|---|---:|---:|
| new | 51 (66) | 152 (174) |
| cancel | 86 (92) | 262 (206) |
| modify | 134 (154) | 352 (327) |

Throughput: 14.42 M msgs/s (v1: 11.45 in the same run, a slow run overall). Cancel and modify
tails are now worse than v1 in the same run, consistent with the extra hop on those paths.

For context, v1 also passes the flash1 benchmark on all five scenarios (`flash1/run.sh`; see the
flash1 section at the end), at about 7–10 M msgs/s on that workload, which differs from this one.

## v2 step 3: intrusive FIFO in the pool (2026-11-13)

Each pooled order now carries its own queue links (`next`/`prev`, 32-bit slot numbers), and a
level is just `{head, tail, total}`. The `std::list` of slot numbers is gone, and the id table
stores the slot directly. Cancel unlinks in O(1) without touching any other allocation.

| | instructions | L1d misses | LL data misses | allocations |
|---|---:|---:|---:|---:|
| v1 | 401 | 5.36 | 0.667 | 1.036 |
| v2 step 2 | 411 | 4.93 | 0.706 | 1.036 |
| v2 step 3 | **308** | **4.75** | 0.696 | **0.518** |

Allocations per message by kind (v1 in brackets): new 0.952 (1.903), cancel 0 (0),
modify 1.001 (2.001).

- **Allocations halved.** A resting order no longer needs a list node. The one allocation left
  is the id table's hash node, which step 4 (a flat id table) targets.
- **−103 instructions per message (−25%) against step 2, and −93 against v1.** Making and
  freeing a list node (the allocator's bookkeeping) is gone, along with the extra hop from
  list node → slot → order.
- **L1d misses down slightly.** LL misses are unchanged, which is expected: the pool and the
  map of levels have the same footprint as before.

Latency, 11 interleaved rounds (v1 in the same run in brackets):

| op | p50 ns | p99 ns |
|---|---:|---:|
| new | 45 (70) | 119 (187) |
| cancel | 73 (97) | 201 (225) |
| modify | 110 (160) | 273 (333) |

Throughput: 16.00 M msgs/s (v1: 11.28 in the same run).
The cancel and modify tails that regressed in step 2 (cancel p99 262 vs v1 206) are now
ahead of v1 in the same run (201 vs 225).

## v2 step 4: flat open-addressing id table (2026-11-15)

`std::unordered_map` (one heap node per resting order) is replaced by `FlatIdMap`: one array of
`{id, entry, occupied}` slots, linear probing, maximum load 0.5, power-of-two size, and
backward-shift deletion (no tombstones). The hash is the splitmix64 finalizer, so ids that
share low bits (e.g. multiples of 4096) still spread out. The entry was reordered to 16 bytes.

**Capacity hint.** From this step, the benchmarks build FastBook with the workload's peak
resting-order count (5,067 here; `make_book` in `bench/workload.h`) instead of the default
65,536. Earlier steps used the default; their structures were insensitive to it, apart from
step 0's `reserve` (see above). A flat table is not: its footprint is set by capacity, and a
good hash spreads lookups evenly over all of it.

| | instructions | L1d misses | LL data misses | allocations |
|---|---:|---:|---:|---:|
| v1 | 401 | 5.36 | 0.667 | 1.036 |
| v2 step 3 | 308 | 4.75 | 0.696 | 0.518 |
| v2 step 4, default capacity (131,072 slots, 4 MB) | 218 | 3.87 | 0.948 | 0.000 |
| v2 step 4, peak capacity (16,384 slots, 512 KB) | **237** | **4.15** | **0.676** | **0.000** |

Allocations per message by kind (v1 in brackets): new 0.001 (1.903), cancel 0 (0),
modify 0.001 (2.001). The remaining 0.001 is `std::map` creating a new price level, which
step 5 targets.

- **No allocation on the hot path.** The hash node was the last per-order allocation.
- **−71 instructions per message against step 3, −164 against v1.** No allocator call on
  rest, and a lookup reads one slot instead of bucket → node.
- **Oversizing the table costs tail latency.** At the default capacity, the table is 4 MB for
  ~5,000 live orders, so almost every lookup lands on a cold cache line: LL misses rose to
  0.948 and `new` p99 was ~210 ns, worse than v1. Sized to the peak, LL misses return to
  step 3's level. It costs 19 more instructions, because probes are longer at load ~0.3 than
  at ~0.04.
- **Hash quality is a correctness-of-performance issue.** With `x ^ (x >> 30)` (no
  multiplies), the map test that inserts 20,000 ids spaced 4,096 apart took 5.2 s in the
  debug build; with the finalizer, 62 ms. The benchmark's ids are sequential, the best case
  for a weak hash, so this does not show in the numbers above.

Latency, 11 interleaved rounds, peak capacity (v1 in the same run in brackets; two runs, the
second shown):

| op | p50 ns | p99 ns |
|---|---:|---:|
| new | 39 (49) | 117 (138) |
| cancel | 38 (68) | 113 (153) |
| modify | 72 (113) | 185 (241) |

Throughput: 22.17 M msgs/s (v1: 15.61 in the same run). v2 is now at or below v1 on every
p50 and p99, in both runs. At the default capacity, in a separate run, v2's p99 was new 216,
cancel 179, modify 218 (v1: 137, 164, 270).

## v2 step 5: sorted-vector price levels, measured and reverted (2026-11-16)

Tried in commit feb293d and reverted: each side's `std::map<Price, Level>` replaced by
`SortedLevels`, one `std::vector` of `{price, level}` sorted worst first, so the best level is
`back()` and a level emptied by a sweep is a `pop_back`. Lookups binary-search with
`std::ranges::lower_bound`. FastBook keeps `std::map` levels.

**Deep-book workload.** The default workload holds only ~13 levels per side, so it cannot show
the cost of shifting a vector. `lob_latency --deep` / `cachegrind.sh 200000 --deep` place 20%
of passive orders 31-5,000 ticks from the touch (~500 levels per side). A scratch
microbenchmark of one deep level insert + erase: vector ~410 ns at 1,000 levels and ~6.7 µs at
10,000, `std::map` ~20-120 ns; one tick behind the best: vector ~2 ns.

| | default: instructions | default: L1d misses | deep: instructions | deep: L1d misses |
|---|---:|---:|---:|---:|
| v1 | 401 | 5.36 | 472 | 6.76 |
| v2 step 4 (`std::map` levels) | **237** | 4.15 | **305** | 5.38 |
| v2 step 5, binary search | 277 | 4.12 | 743 | 4.64 |
| v2 step 5, linear scan from the back (scratch) | 250 | 4.12 | 1,090 | 4.59 |

LL misses were unchanged (0.676-0.681) and allocations went from 0.001 to 0.000 per message.

- **Default book: no win.** About 13 levels per side fit in L1 as `std::map` nodes, and the map
  reaches the best level in O(1) (it caches its leftmost node). The binary searches cost ~60
  instructions per message, so the vector ran 40 more instructions per message for almost no
  change in L1 misses.
- **Deep book: much worse.** `memmove` was 52% of all instructions (~380 per message): every
  level created or emptied deep in the book shifts every level behind it. L1 misses fell
  (contiguous storage), but not enough to pay for the copying.

Latency, 11 interleaved rounds (v1 in the same run in brackets):

| workload, op | step 5 p50 ns | step 5 p99 ns | step 4 p50 / p99, earlier run |
|---|---:|---:|---:|
| default, new | 41 (49) | 124 (130) | 39 / 117 |
| default, cancel | 42 (68) | 123 (138) | 38 / 113 |
| default, modify | 78 (112) | 192 (219) | 72 / 185 |
| deep, new | 53 (63) | 211 (228) | 52 / 180 |
| deep, cancel | 58 (79) | 258 (265) | 54 / 181 |
| deep, modify | 109 (140) | 423 (426) | 102 / 301 |

Throughput against v1 in the same run: default 1.32x (step 4: 1.42x), deep 1.15x (step 4:
1.31x). The deep-book step 4 baseline (same machine, earlier run): v2 p99.9 new / cancel / modify
297 / 314 / 512 ns, 14.51 M msgs/s vs v1 11.05.

The textbook argument for a sorted vector (contiguous, fewer pointer hops) assumes the tree's
nodes are cold. Here the hot levels already sit in cache, and the structure's worst case
(shifting) grows with book depth. A structure that bounds the shift (e.g. a vector near the
touch and a tree beyond it) would be the next thing to try; it is not part of this plan.

## v2 step 6: one id-table operation per message (2026-11-16)

Two changes, each removing a second hash probe for the same id.

- **New orders claim their id before matching.** A GTC order allocates its pool slot and inserts
  its id-table entry up front; a failed insert *is* the duplicate check, so validation no longer
  probes with `contains`. If the order then fills completely, the claim is undone. IOC and
  market orders never rest, so they keep the `contains` check and claim nothing. Modify claims
  the new id before matching too, so "no slot" means only "this order never rests".
- **Cancel and modify erase through the entry they found** (`FlatIdMap::erase(Value*)`), so the
  backward shift starts at the slot `find` returned instead of probing for the id again.

| | default: instructions | default: L1d misses | deep: instructions | deep: L1d misses |
|---|---:|---:|---:|---:|
| v1 | 401 | 5.36 | 472 | 6.76 |
| v2 step 4 | 237 | 4.15 | 305 | 5.38 |
| v2 step 6, claim before matching only | 244 | 4.16 | 314 | — |
| v2 step 6, with clean-ups and erase by entry | **232** | **4.16** | **301** | **5.38** |

LL misses were unchanged (0.676 default, 0.681 deep). Allocations per message: new 0.001,
cancel 0, modify 0.001.

- **The first version was a regression.** The saved `contains` was worth ~9 instructions per
  message inside `flat_id_map.h`, but the code added around it cost ~21: `claim_id` was called
  from two places and not inlined, `std::optional<Slot>` added a flag and branches, and "no slot"
  meant two different things. Using the existing `kNoSlot` sentinel and claiming in one place
  for both callers turned −7 into +5.
- **Fewer instructions, not fewer cache misses.** The second probe always hit the cache line the
  first had just loaded, so removing it saves computation (hash, compare) but no memory traffic.
  This is why the miss columns do not move.

Latency, 11 interleaved rounds (v1 in the same run in brackets). This run's machine was slower
than step 4's (v1 throughput 13.06 vs 15.61 M msgs/s), so compare against v1 in the same run,
not against step 4's absolute numbers:

| workload, op | p50 ns | p99 ns |
|---|---:|---:|
| default, new | 38 (60) | 122 (158) |
| default, cancel | 51 (82) | 131 (173) |
| default, modify | 84 (137) | 202 (268) |
| deep, new | 48 (74) | 186 (235) |
| deep, cancel | 64 (92) | 211 (268) |
| deep, modify | 111 (163) | 340 (423) |

Throughput against v1 in the same run: default 19.14 vs 13.06 M msgs/s (1.47x; step 4: 1.42x),
deep 13.14 vs 9.55 (1.38x; step 4: 1.31x). A gain this small is within run-to-run noise in wall
time; the cachegrind counts above are the reliable measure for it.

## flash1: external correctness check, v1 and v2 (2026-11-16)

`flash1/run.sh` builds the flash1 adapter once per book (`-DLOB_FLASH1_BOOK=1` or `2`, `-O3
-march=native -DNDEBUG`) and runs the harness on all five scenarios. `perf` checks the SHA-256 of
the full report stream against the consensus; `audit` checks 192 best bid/ask/depth samples
against liquibook. (`static` has no audit: liquibook itself exceeds the harness's 120 s alarm on
that scenario.)

| scenario | v1 M msgs/s | v2 M msgs/s | v2 / v1 | verdict (both books) |
|---|---:|---:|---:|---|
| static | 9.92 | 12.85 | 1.30x | VALID (perf) |
| normal | **7.28** | 10.55 | 1.45x | VALID (perf + audit) |
| swing-25 | 7.52 | **10.31** | 1.37x | VALID (perf + audit) |
| swing-40 | 9.68 | 11.63 | 1.20x | VALID (perf + audit) |
| flash-crash | 9.86 | 11.04 | 1.12x | VALID (perf + audit) |

**Worst-case throughput** (the figure flash1 reports): v1 7.28 M msgs/s (normal), v2 10.31 M
msgs/s (swing-25), 1.42x. One run per cell, on WSL2: indicative only. v2 also passes `perf` on
static, normal, swing-40 and flash-crash with its debug `assert`s enabled (no `-DNDEBUG`).

The gap is smaller than on our own workload (1.47x), most likely because each message here also pays for the
adapter's translation and for pushing every report into the harness's queue, which costs the same
for both books, so the book is a smaller share of the total. FastBook runs with its default
capacity hint (65,536), not one sized to these workloads.
