#!/usr/bin/env bash
# Execute precise maintainer-requested native SwiftNIO tests and raw macOS kernel probe.
# Does not merge, tag, modify test assertions, or claim that the full suite is green.
set -euo pipefail

PR_SHA="4867bc8d708a51ca7e81759ab03e16a3cbd176a6"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
cd "$ROOT"
DIAG_SHA="$(git rev-parse HEAD)"

if [[ -n "$(git status --porcelain)" ]]; then
  echo "Checkout contains local changes; refusing to switch commits" >&2
  git status --short >&2
  exit 2
fi

OUT="$HOME/Desktop/SwiftNIO-Reviewer-Acceptance-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT"

restore() {
  git switch --detach "$DIAG_SHA" >/dev/null 2>&1 || true
}
trap restore EXIT

{
  echo "diagnostic_sha=$DIAG_SHA"
  echo "upstream_pr_sha=$PR_SHA"
  echo "=== machine / toolchain ==="
  sw_vers
  uname -m
  xcodebuild -version
  swift --version
} >"$OUT/metadata.txt" 2>&1

git fetch origin "$PR_SHA"
git switch --detach "$PR_SHA"

test "$(git rev-parse HEAD)" = "$PR_SHA"
test -z "$(git status --porcelain)"

FILTER='testIPv4PeerEOFArrivesWithAutoReadDisabledOnMacOS27|testIPv6PeerEOFArrivesWithAutoReadDisabledOnMacOS27|testEarlyEOFEligibilityIncludesIPv4AndIPv6ButNotUnixOrMissingAddress'

echo "=== Reviewer IPv4 / IPv6 / address-family checks, Debug ==="
swift test -j 2 --filter "$FILTER" >"$OUT/swiftnio-debug.log" 2>&1 || {
  tail -n 80 "$OUT/swiftnio-debug.log" >&2; exit 1;
}

echo "=== Same tests, optimized Release ==="
swift test -c release -j 2 --filter "$FILTER" >"$OUT/swiftnio-release.log" 2>&1 || {
  tail -n 80 "$OUT/swiftnio-release.log" >&2; exit 1;
}

for mode in debug release; do
  for name in testIPv4PeerEOFArrivesWithAutoReadDisabledOnMacOS27 testIPv6PeerEOFArrivesWithAutoReadDisabledOnMacOS27; do
    if ! grep -Fq "$name" "$OUT/swiftnio-$mode.log"; then
      echo "No evidence that $name executed in $mode" >&2
      exit 1
    fi
  done
done

echo "=== Strict format lint on the actual PR diff ==="
xcrun swift-format lint --strict \
  Sources/NIOPosix/BaseSocketChannel.swift \
  Sources/NIOPosix/SelectorGeneric.swift \
  Tests/NIOPosixTests/SocketChannelTest.swift \
  Tests/NIOPosixTests/SyscallAbstractionLayer.swift \
  >"$OUT/swift-format.log" 2>&1 || {
    cat "$OUT/swift-format.log" >&2; exit 1;
  }

test "$(git rev-parse HEAD)" = "$PR_SHA"
test -z "$(git status --porcelain)"

tar -czf "$OUT.tar.gz" -C "$(dirname "$OUT")" "$(basename "$OUT")"
shasum -a 256 "$OUT.tar.gz" >"$OUT.sha256"
echo "Reviewer-test archive: $OUT.tar.gz"

echo "=== Restore diagnostic commit for standalone, non-NIO kernel experiments ==="
git switch --detach "$DIAG_SHA"
test "$(git rev-parse HEAD)" = "$DIAG_SHA"
test -z "$(git status --porcelain)"

bash dev/diagnostics/macos27-kqueue-eof/run.sh 3

echo "=== Targeted maintainer tests and raw kernel diagnostic finished ==="
echo "NOTE: existing full NIOPosix saturation test remains a known RED gate until fixed."
