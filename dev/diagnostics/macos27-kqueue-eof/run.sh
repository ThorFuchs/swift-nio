#!/usr/bin/env bash
# Local, dependency-free macOS socket/kqueue diagnostic. No privileged actions.
set -euo pipefail

REPETITIONS=3
if [[ $# -ge 1 ]]; then REPETITIONS="$1"; fi
case "$REPETITIONS" in
  ''|*[!0-9]*) echo "Expected repetition count 1..12" >&2; exit 2 ;;
esac
if (( REPETITIONS < 1 || REPETITIONS > 12 )); then
  echo "Expected repetition count 1..12" >&2
  exit 2
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This is a macOS-only kqueue probe" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
OUT="$HOME/Desktop/SwiftNIO-KQueue-Proof-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT"

{
  echo "=== exact source provenance ==="
  git -C "$ROOT" rev-parse HEAD
  git -C "$ROOT" status --porcelain
  git -C "$ROOT" remote -v | sed -E 's#https://[^/@]+@#https://[REDACTED]@#g'
  echo "=== OS ==="
  sw_vers
  uname -m
  sysctl kern.osrelease kern.osversion || true
  echo "=== build tools ==="
  xcodebuild -version
  xcrun --sdk macosx clang --version
  echo "=== reproducible inputs ==="
  shasum -a 256 "$SCRIPT_DIR/kqueue-eof-probe.c" "$SCRIPT_DIR/run.sh"
  echo "repetitions=$REPETITIONS"
  echo "NOTE: this uses only public EVFILT_EXCEPT, NOTE_LOWAT and EV_CLEAR."
} >"$OUT/metadata.txt" 2>&1

cp "$SCRIPT_DIR/kqueue-eof-probe.c" "$OUT/kqueue-eof-probe.c"
cp "$SCRIPT_DIR/run.sh" "$OUT/run.sh"

echo "Building public-kqueue probe..."
if ! xcrun --sdk macosx clang -std=c11 -O2 -Wall -Wextra -Werror \
  "$SCRIPT_DIR/kqueue-eof-probe.c" -o "$OUT/kqueue-eof-probe" \
  >"$OUT/compile.log" 2>&1; then
  echo "Compile error, see $OUT/compile.log" >&2
  cat "$OUT/compile.log" >&2
  exit 1
fi

echo "Running LEVEL vs EV_CLEAR / IPv4 vs IPv6 vs UDS / empty, small, full, reset..."
"$OUT/kqueue-eof-probe" "$REPETITIONS" >"$OUT/results.tsv" 2>"$OUT/probe-stderr.log"

EXPECTED=$(( REPETITIONS * 2 * 3 * 4 ))
ACTUAL="$(awk 'END { print NR - 1 }' "$OUT/results.tsv")"
if [[ "$ACTUAL" != "$EXPECTED" ]]; then
  echo "Unexpected scenario count: $ACTUAL, wanted $EXPECTED" >&2
  exit 1
fi

# This is a diagnostic, NOT a pass/fail CI gate. "OBSERVED" does not mean
# zero spurious events. Preserve every individual row and non-EOF count.
{
  echo "expected_scenarios=$EXPECTED"
  echo "recorded_scenarios=$ACTUAL"
  awk -F '\t' 'NR>1 {
      status[$21]++;
      k=$1 "|" $2 "|" $3;
      cases[k]++;
      if ($8+0>0) spurious[k]++;
      if ($14+0>0) eof_without_recv[k]++;
      if ($16+0>0) eof_after_drain[k]++;
      if ($10+0>0 || $17+0>0) capped[k]++;
      if ($3=="full" && $6+0<$5+0) fullness_unknown[k]++;
    }
    END {
      for (s in status) print "status " s "=" status[s];
      for (k in cases)
        print "case " k " total=" cases[k] " pre_nonEOF=" (spurious[k]+0) " no_recv_EOF=" (eof_without_recv[k]+0) " post_drain_EOF=" (eof_after_drain[k]+0) " capped=" (capped[k]+0) " full_unconfirmed=" (fullness_unknown[k]+0);
    }' "$OUT/results.tsv" | sort
} >"$OUT/summary.txt"

tar -czf "$OUT.tar.gz" -C "$(dirname "$OUT")" "$(basename "$OUT")"
shasum -a 256 "$OUT.tar.gz" | tee "$OUT.sha256"

cat "$OUT/summary.txt"
echo
echo "Archive: $OUT.tar.gz"
echo "Full raw observations: $OUT/results.tsv"
echo "This does not modify the upstream PR or any SwiftNIO test."
