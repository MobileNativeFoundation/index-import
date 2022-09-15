#!/bin/bash

set -euo pipefail
set -x

mkdir -p /tmp/indexstorebuild
git clone --branch swift/release/5.7 --depth 1 https://github.com/apple/llvm-project.git

mkdir -p llvm-project/build
cd llvm-project/build

cmake ../llvm -G Ninja -DLLVM_ENABLE_PROJECTS=clang
ninja libIndexStore.dylib
