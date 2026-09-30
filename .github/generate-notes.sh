#!/bin/bash

set -euo pipefail

readonly swift_version=$1
shift

readonly upstream_url="https://github.com/swiftlang/llvm-project/releases/tag/swift-$swift_version-RELEASE"

archives_sha=$(shasum -a 256 "$@")

cat <<EOF
The binaries included with this release were built against Swift $swift_version at [this tag]($upstream_url).

sha256:
\`\`\`
$archives_sha
\`\`\`
EOF
