# orderbook

A single-instrument limit order book and matching engine in C++20.

<!-- One or two sentences: what it is, and the headline verified result. -->

## Results

<!-- Table: v1 (baseline) vs v2 (fast), p50/p99 per operation, cachegrind misses/op.
     External correctness benchmark verdict and worst-case throughput. -->

## Design

<!-- The v1 -> v2 choices and their trade-offs: price levels, order queues, pool, id index. -->

## Correctness

<!-- Rule tests, differential tests, sanitizers, external benchmark. -->

## Benchmark methodology

<!-- Workload, timing, interleaved rounds, pinning, WSL caveat, cachegrind. -->

## Extending

<!-- Where FOK / post-only / iceberg / stop orders, pro-rata matching and
     SPSC queues / one-book-per-instrument would plug in. -->

## Limitations

## Build and run

Requires Linux (or WSL), CMake 3.21+, Ninja and GCC 14 (or another C++20 compiler).

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug   # tests, ASan + UBSan
cmake --preset release && cmake --build --preset release                      # optimised build
```
