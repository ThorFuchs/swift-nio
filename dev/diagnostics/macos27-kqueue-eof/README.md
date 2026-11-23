# macOS 27 — raw kqueue early-EOF and receive-buffer saturation matrix

**Status: diagnostic only, unqualified and not a proposed production SwiftNIO change.**
This is a standalone, unprivileged C11 program. It uses public socket and kqueue interfaces only (EVFILT_EXCEPT, NOTE_LOWAT, EV_CLEAR, EV_EOF). It does not use private EVFILT_SOCK or NOTE_READCLOSED constants, and does not modify any SwiftNIO tests.

## Why it exists

In the macOS 27 upstream early-EOF PR apple/swift-nio#3783, the existing NIOPosix regression test
StreamChannelTest.testLotsOfWritesWhilstOtherSideNotReading produced unexpected EVFILT_EXCEPT events on
an IPv4 TCP socket with autoRead disabled and a full receive buffer. The A/B attempt to add EV_CLEAR
also produced spurious non-EOF events (five/six baseline trials green, four/six EV_CLEAR trials green,
in a separate run). This is distinct from the UNIX-domain-socket issue, which the PR excludes.

SwiftNIO configures its EXCEPT filter with NOTE_LOWAT and data=Int.max. The publicly accessible XNU
implementation of filt_soread_common clamps a requested NOTE_LOWAT level to the actual receive-buffer
high-water mark (so_rcv.sb_hiwat), and the same routine serves EVFILT_EXCEPT on sockets. Thus Int.max
does not guarantee that a filled buffer can never trigger raw EXCEPT readiness. The exact running
macOS 27 beta kernel source has not been byte-verified against that public XNU source.

## Goals, not assertions to be weakened

1. **IPv4 and IPv6:** Compare no-read EOF on connected TCP with empty and small unread queues.
2. **Full receive buffer:** Record FIONREAD and the actual SO_RCVBUF result, send until the receiver
   is apparently full (or the bounded fill attempt ends), and count raw non-EOF EXCEPT notifications
   while the peer remains alive. NO receive calls occur during this observation.
3. **Close without reading:** Issue FIN via shutdown(SHUT_WR), and measure whether raw EV_EOF arrives
   before any receive call.
4. **Close after draining:** If TCP receive window saturation delays delivery of FIN, drain the
   receive queue and observe subsequent raw EV_EOF. Do not assume FIN is physically available
   before the receiver has made space for outstanding TCP data.
5. **RST:** Separately record abortive peer close using SO_LINGER=0.
6. **UDS control:** Measure a socketpair under the same conditions. UDS raw EXCEPT may be noisy;
   SwiftNIO must not enable readEOF on this family merely because the OS is macOS 27.
7. **Flags:** Compare regular level-triggered registration with EV_CLEAR. Measure kernel
   notifications, not only the events surfaced by SwiftNIO's selector-to-channel conversion.

The probe measures non-EOF events rather than filtering them out. Every case uses fresh sockets
and a fresh kqueue. The observation window and event cap are bounded to avoid a pathological loop
in the probe itself. A capped count is a lower bound, not a complete kernel wakeup count.
The actual receive high-water mark is not always equal to SO_RCVBUF; FIONREAD/capacity is a
measurement heuristic, not proof of internal TCP state. FULL_NOT_CONFIRMED is deliberately
reported when queued bytes never reach the SO_RCVBUF value. No performance claim should be made
from a small number of trials.

## Mac Studio usage

**One-command reviewer evidence:** In a clean detached checkout of this diagnostic branch,
run the orchestrator below. It temporarily checks out the exact upstream PR SHA,
runs the explicitly requested IPv4, IPv6 and address-family tests in Debug and optimized
Release, checks the four changed Swift files with strict formatter lint, and then
restores the diagnostic SHA and runs the independent kernel matrix. It does **not**
run or greenwash the already known failing full NIOPosix saturation case.

    bash dev/diagnostics/macos27-kqueue-eof/run-review-evidence.sh

For the **standalone kernel matrix only**, run:

    bash dev/diagnostics/macos27-kqueue-eof/run.sh 3

The runner checks macOS, builds with the installed Apple clang and strict warnings, then creates
an archive on the Desktop containing metadata.txt (Git SHA, OS, compiler, hashes), source snapshots,
compile.log, results.tsv (each individual case), summary.txt and stderr. No root access or network
connection is needed beyond local loopback sockets. No user files are modified outside that
new Desktop evidence directory. The diagnostic source file is not compiled by SwiftPM.

The source is also runnable directly:

    xcrun --sdk macosx clang -std=c11 -O2 -Wall -Wextra -Werror kqueue-eof-probe.c -o kqueue-eof-probe
    ./kqueue-eof-probe 3 > results.tsv

The **results.tsv** field definitions are: mode, address family, scenario, iteration,
SO_RCVBUF capacity, queued bytes before shutdown, sender bytes accepted, pre-close non-EOF and
EOF counts, pre-close saturation cap, post-close non-EOF count, first spurious flags/data,
post-close EOF, after-drain non-EOF/EOF counts, after-drain saturation cap, drained byte count,
whether recv returned zero, whether recv returned ECONNRESET, and a diagnostic status.

Do not report a "PASS" solely because the process exits 0; it is a measurement tool whose
normal exit does not imply a green SwiftNIO test suite. Specifically, OBSERVED can coexist with
spurious events. Check the event columns.

## Upstream review mapping

Latest review of apple/swift-nio#3783 (2026-11-23): Felix Greis requests an explicit **IPv4** real
socket integration test with autoRead disabled, analogous to existing IPv6; the pre-existing
UDS/full-buffer and nil-address coverage satisfy his earlier requests. The requested IPv4 test
has been added separately to the actual PR branch as
SocketChannelTest.testIPv4PeerEOFArrivesWithAutoReadDisabledOnMacOS27. The IPv4 and IPv6 cases
share a single activation barrier/helper and identical assertions.

The full SwiftNIO run on the prior PR head ed939afc... was RED due to nine raw EXCEPT
assertions in one StreamChannelTest (NIOPosix 764 executed, 37 skipped). The A/B experiment
with EV_CLEAR failed to eliminate the regression. This diagnostic is aimed at the next
design decision; it does not certify this PR or authorize an upstream merge.

## Next acceptance gate

After collecting raw data, agree on a **publicly supported** kernel strategy with maintainers,
add a deterministic TCP-full-queue / peer-close liveness regression that does not suppress
unwanted raw wakeups, and requalify IPv4+IPv6 empty/buffered/full, UDS, nil-local-address and
listening/datagram exclusion. Then run the *entire* SwiftNIO suite, strict formatter, Linux
CI and downstream HTTP consumer tests on an immutable revision. Do not remove three
downstream early-EOF known-issue annotations before that evidence exists.

Related:
- https://github.com/apple/swift-nio/pull/3783
- https://github.com/ThorFuchs/Swift-MCP-Server-HTTP/issues/12
