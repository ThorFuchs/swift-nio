#!/usr/bin/env bash
# Standalone concurrent macOS TCP/kqueue stress, not a SwiftNIO build or PR gate.
set -euo pipefail
if [[ "$(uname -s)" != Darwin ]]; then
  echo "Requires macOS" >&2; exit 2
fi
REPS=3
POLL_MS=1600
if [[ $# -ge 1 ]]; then REPS="$1"; fi
if [[ $# -ge 2 ]]; then POLL_MS="$2"; fi
case "$REPS:$POLL_MS" in *[!0-9:]* ) echo "Expected numeric arguments" >&2; exit 2;; esac
if (( REPS<1 || REPS>12 || POLL_MS<100 || POLL_MS>5000 )); then
  echo "Expected repetitions 1..12 and duration 100..5000ms" >&2; exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
OUT="$HOME/Desktop/SwiftNIO-TCP-EXCEPT-Live-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT"

{
  echo "=== Source / machine ==="
  git -C "$ROOT" rev-parse HEAD
  git -C "$ROOT" status --porcelain
  sw_vers
  uname -m
  sysctl kern.osrelease kern.osversion || true
  xcodebuild -version
  xcrun --sdk macosx clang --version
  shasum -a 256 "$SCRIPT_DIR/tcp-except-live-stress.c" "$SCRIPT_DIR/run-live-stress.sh"
  echo "repetitions=$REPS"
  echo "poll_ms=$POLL_MS"
} > "$OUT/metadata.txt" 2>&1

cp "$SCRIPT_DIR/tcp-except-live-stress.c" "$OUT/tcp-except-live-stress.c"
cp "$SCRIPT_DIR/run-live-stress.sh" "$OUT/run-live-stress.sh"

if ! xcrun --sdk macosx clang -std=c11 -O2 -Wall -Wextra -Werror -pthread \
   "$SCRIPT_DIR/tcp-except-live-stress.c" -o "$OUT/tcp-except-live-stress" \
   > "$OUT/compile.log" 2>&1; then
   cat "$OUT/compile.log" >&2
   echo "Probe compile failed; no SwiftNIO source/test failure is implied." >&2
   exit 1
fi
echo "Live saturation: IPv4/IPv6, native/512KiB, LEVEL/EV_CLEAR, $REPS repetitions..."
"$OUT/tcp-except-live-stress" "$REPS" "$POLL_MS" \
    >"$OUT/results.tsv" 2>"$OUT/probe-stderr.log"

ACTUAL="$(awk 'END {print NR - 1}' "$OUT/results.tsv")"
EXPECTED=$(( REPS * 2 * 2 * 2 ))
if [[ "$ACTUAL" != "$EXPECTED" ]]; then
  echo "Expected $EXPECTED scenarios, received $ACTUAL" >&2
  exit 1
fi

{
  echo "expected_scenarios=$EXPECTED"
  echo "recorded_scenarios=$ACTUAL"
  awk -F '\t' 'NR>1 {
    key=$1 "|" $2 "|" $3;
    count[key]++;
    if ($7+0>maxqueue[key]) maxqueue[key]=$7+0;
    if ($11+0>0) with_spurious[key]++;
    if ($10+0>0) with_cap[key]++;
    if ($16+0>0) pre_read_eof[key]++;
    if ($17+0>0) eof_after_drain[key]++;
    if ($20+0>0) recv_eof[key]++;
    if ($21+0>0) errors[key]++;
  } END {
    for (k in count)
      print k " trials=" count[k] " max_queued=" maxqueue[k] " pre_non_eof_trials=" (with_spurious[k]+0) " capped_trials=" (with_cap[k]+0) " raw_eof_before_drain=" (pre_read_eof[k]+0) " raw_eof_after_drain=" (eof_after_drain[k]+0) " recv_eof=" (recv_eof[k]+0) " send_error_trials=" (errors[k]+0);
  }' "$OUT/results.tsv" | sort
} >"$OUT/summary.txt"

tar -czf "$OUT.tar.gz" -C "$(dirname "$OUT")" "$(basename "$OUT")"
shasum -a 256 "$OUT.tar.gz" | tee "$OUT.sha256"
cat "$OUT/summary.txt"
echo
echo "Archive: $OUT.tar.gz"
echo "This is a diagnostic: a successful exit is NOT a pass for the NIO saturation regression."
