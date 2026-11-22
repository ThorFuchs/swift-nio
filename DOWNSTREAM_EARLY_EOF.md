# Downstream macOS 27 TCP early-EOF patch (RC qualification)

This branch is a **temporary downstream qualification branch**, not an official SwiftNIO release or a published version tag.

- Official upstream base: [apple/swift-nio 2.105.0](https://github.com/apple/swift-nio/releases/tag/2.105.0), commit `2f58d8ef84438d6517fc78bcea2f8527e3c72936`.
- Original patch: [apple/swift-nio PR #3783](https://github.com/apple/swift-nio/pull/3783), submitted from [ThorFuchs/swift-nio:darwin-27-early-eof](https://github.com/ThorFuchs/swift-nio/tree/darwin-27-early-eof), commit `80ffbc5932b54a29cb81b57f47c5f953250a5c11`.
- Files intentionally modified **only**: `Sources/NIOPosix/SelectorGeneric.swift` and `Sources/NIOPosix/BaseSocketChannel.swift`. Both files were identical between upstream 2.105.0 and the original PR base `b440f7b`; no unrelated commits are included in this branch.
- Behavior: `readEOF` on IPv4/IPv6 TCP channels on macOS 27+, not on older macOS, other unverified Darwin platforms, or UNIX-domain sockets (known spurious-event behavior). Linux is unchanged.

## Validation

Run `bash scripts/verify-macos27-early-eof.sh` on macOS 27 with Swift 6.4.2. Test the downstream HTTP's three originally masked cancellation/connection lifecycle tests **without** `withKnownIssue`, plus optimized HTTP ARM64 fixture links and both official server/client referees. Tests on this branch are **not yet certified green**.

## Distribution constraint

The commit does not automatically replace the canonical `apple/swift-nio` version in every external SwiftPM consumer. `Package.resolved` and SwiftPM mirrors in a library's checkout do not become enforceable policy for dependants. Require a fresh top-level consumer resolution proof before any HTTP 1.0 release claim. A developer-side mirror or local override is a test setup, not an automatically distributed binary or an upstream release.

No forked SemVer version tag is authorized by this branch. On upstream release that includes PR #3783, remove the override, return to `apple/swift-nio`, and re-run the full qualification suite.
