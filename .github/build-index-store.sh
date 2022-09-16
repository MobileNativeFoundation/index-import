#!/bin/bash

set -euo pipefail
set -x

rm -rf /tmp/indexstorebuild
mkdir -p /tmp/indexstorebuild
cd /tmp/indexstorebuild
git clone --branch swift/release/5.7 --depth 1 https://github.com/apple/llvm-project.git

cd llvm-project
mkdir -p build

cmake -B build llvm -DLLVM_ENABLE_PROJECTS=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target IndexStore FileCheck
