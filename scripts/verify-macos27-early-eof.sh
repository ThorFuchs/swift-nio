#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run the original SwiftNIO suite on a macOS 27 / Swift 6.4.2 host.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ "$(uname -s)" != "Darwin" ]] || [[ "$(sw_vers -productVersion | cut -d. -f1)" -lt 27 ]]; then
  echo "This qualification targets macOS 27+; cannot claim early EOF on this host" >&2
  exit 2
fi
swift --version
swift test --filter NIOPosixTests
swift test
