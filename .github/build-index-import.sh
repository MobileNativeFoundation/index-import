#!/bin/bash

set -euo pipefail
set -x

mkdir build
cd build

PATH=/tmp/indexstorebuild/llvm-project/build:$PATH cmake ../ -G Ninja
ninja
