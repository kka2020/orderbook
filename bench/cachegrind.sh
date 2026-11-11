#!/usr/bin/env bash
# Deterministic "why" numbers: instructions and simulated cache misses per message.
#
#   bench/cachegrind.sh [messages]     default 200,000 (cachegrind runs ~50x slower than native)
#
# cachegrind simulates the CPU's caches instead of reading hardware counters, so the counts are
# the same on every run and unaffected by WSL. Only code inside replay_once() (the timed replay)
# is counted (it brackets itself with cachegrind start/stop markers; valgrind starts with
# instrumentation off): workload generation and the prefill are excluded.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/release/bench/lob_latency"
MESSAGES="${1:-200000}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

cmake --build --preset release --target lob_latency >/dev/null

if ! valgrind --tool=cachegrind --cache-sim=yes --instr-at-start=no \
        --cachegrind-out-file="$OUT/cg.out" "$BIN" --replay-once "$MESSAGES" 2>"$OUT/valgrind.log"; then
    cat "$OUT/valgrind.log" >&2
    exit 1
fi

# The raw output file has an "events:" line naming the columns and a "summary:" line with the
# totals. Ir = instructions; D1mr/D1mw = L1 data read/write misses; DLmr/DLmw = last-level data
# read/write misses (the expensive ones: they go to DRAM).
awk -v n="$MESSAGES" '
    /^events:/  { for (i = 2; i <= NF; i++) col[$i] = i }
    /^summary:/ {
        printf "per message: %.0f instructions, %.2f L1d misses, %.3f LL data misses\n",
               $col["Ir"] / n, ($col["D1mr"] + $col["D1mw"]) / n, ($col["DLmr"] + $col["DLmw"]) / n
    }' "$OUT/cg.out"
