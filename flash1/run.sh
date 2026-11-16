#!/usr/bin/env bash
# Build the flash1 adapter against a pinned copy of the harness and check it on every scenario.
#
#   flash1/run.sh                    # both books, all five scenarios, perf (report hash) + audit (book state)
#   flash1/run.sh normal swing-25    # just these scenarios
#   BOOKS=v2 flash1/run.sh           # just one book (v1 = BaselineBook, v2 = FastBook)
#
# Extra harness flags (e.g. --write-canonical-output) can be passed via HARNESS_ARGS.
# The harness checkout lives in flash1/.harness (gitignored) and is reused between runs.
set -euo pipefail

FLASH1_URL="https://github.com/flash1-dev/matching-engine-benchmark.git"
FLASH1_REF="424e4e5649652febb5b0c4c80859d74c4654131e"  # 2026-10-16

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
H="$HERE/.harness"
CXX="${CXX:-g++-14}"

if [[ ! -d "$H/.git" ]]; then
    git clone --quiet "$FLASH1_URL" "$H"
fi
git -C "$H" fetch --quiet origin "$FLASH1_REF" 2>/dev/null || true
git -C "$H" checkout --quiet "$FLASH1_REF"

cd "$H"

# Harness + generator (built once).
if [[ ! -x harness ]]; then
    echo "== building harness"
    make >/dev/null
fi

# liquibook is the baseline the --mode audit state check compares against.
if [[ ! -f liquibook_adapter.so ]]; then
    echo "== building liquibook baseline (for the state audit)"
    bash scripts/build_baselines.sh liquibook >/dev/null
fi

read -r -a BOOKS <<<"${BOOKS:-v1 v2}"

# Our engine: same flags flash1 documents, plus our warning set and -DNDEBUG (as the release
# preset), one .so per book. Always rebuilt.
for book in "${BOOKS[@]}"; do
    case "$book" in
        v1) n=1 ;;
        v2) n=2 ;;
        *) echo "unknown book '$book' (expected v1 or v2)" >&2; exit 2 ;;
    esac
    echo "== building lob_$book.so"
    "$CXX" -std=c++20 -O3 -march=native -DNDEBUG -fPIC -shared \
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror \
        -DLOB_FLASH1_BOOK="$n" -I api -I "$ROOT/include" \
        "$HERE/adapter.cpp" -o "lob_$book.so"
done

SCENARIOS=("$@")
if [[ ${#SCENARIOS[@]} -eq 0 ]]; then
    SCENARIOS=(static normal swing-25 swing-40 flash-crash)
fi

LOG="$HERE/.harness/lob_runs.log"
: > "$LOG"
fail=0
for book in "${BOOKS[@]}"; do
for s in "${SCENARIOS[@]}"; do
    for mode in perf audit; do
        # The audit replays the workload through liquibook under a fixed 120 s alarm. liquibook
        # runs `static` (the deepest book) at ~0.02 M msgs/s, ~100 s, so that audit times out
        # whatever engine is under test. The static perf run still checks the full report hash.
        if [[ "$s" == static && "$mode" == audit ]]; then
            printf '%-3s %-12s %-6s %s\n' "$book" "$s" "$mode" "SKIPPED (liquibook baseline exceeds the harness's 120 s audit alarm)"
            continue
        fi
        out="$(./harness --engine "./lob_$book.so" --scenario "$s" --mode "$mode" ${HARNESS_ARGS:-} 2>&1)" || true
        printf '%s\n' "== $book / $s / $mode" "$out" >> "$LOG"
        verdict="$(grep -o 'Verdict: [A-Z]*' <<<"$out" | tail -1 || true)"
        detail="$(grep -E 'Throughput:|Status:|State audit:|ERROR' <<<"$out" | sed 's/^ *//' | paste -sd ' ' - || true)"
        printf '%-3s %-12s %-6s %-18s %s\n' "$book" "$s" "$mode" "${verdict:-Verdict: ERROR}" "$detail"
        [[ "$verdict" == "Verdict: VALID" ]] || fail=1
    done
done
done

echo "Full harness output: $LOG"
exit "$fail"
