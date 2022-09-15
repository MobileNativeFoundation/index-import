#!/bin/bash

set -euo pipefail
set -x

mkdir build

PATH=/tmp/indexstorebuild/llvm-project/build:$PATH cmake -B build
cmake --build build
