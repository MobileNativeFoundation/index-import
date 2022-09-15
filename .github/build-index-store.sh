#!/bin/bash

set -euo pipefail
set -x

mkdir -p /tmp/indexstorebuild
git clone --branch swift/release/5.7 --depth 1 https://github.com/apple/llvm-project.git

cd llvm-project
mkdir -p build

cmake -B build llvm -DLLVM_ENABLE_PROJECTS=clang
cmake --build build libIndexStore FileCheck
