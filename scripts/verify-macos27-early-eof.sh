#!/bin/bash
##===----------------------------------------------------------------------===##
##
## This source file is part of the SwiftNIO open source project
##
## Copyright (c) 2026 Apple Inc. and the SwiftNIO project authors
## Licensed under Apache License v2.0
##
## See LICENSE.txt for license information
## See CONTRIBUTORS.txt for the list of SwiftNIO project authors
##
## SPDX-License-Identifier: Apache-2.0
##
##===----------------------------------------------------------------------===##
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
