#!/bin/bash

set -euo pipefail

readonly swift_version=$1
readonly macos_archive=$2

readonly upstream_url="https://github.com/swiftlang/llvm-project/releases/tag/swift-$swift_version-RELEASE"

macos_sha=$(shasum -a 256 "$macos_archive")

cat <<EOF
The binary included with this release was built against Swift $swift_version at [this tag]($upstream_url).

sha256:
\`\`\`
$macos_sha
\`\`\`
EOF
