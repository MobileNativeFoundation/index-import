#!/bin/bash

set -euo pipefail

readonly base_dir=$(dirname "$0")

pushd "$base_dir"/clang >/dev/null

# Produce the index.
xcrun clang -fsyntax-only -index-store-path input input.c

# Test index-import by matching its verbose output.
# See https://llvm.org/docs/CommandGuide/FileCheck.html
../../build/index-import -V \
  -remap input.c.o=output.c.o \
  -remap "$(pwd)"=/fake/working/dir \
  input output \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output.c.o-1I92L511L7IRP >/dev/null
ls output/v5/records/CS/input.c-1GAVGMEKRGFCS >/dev/null

# Check that the record files are identical.
cmp {input,output}/v5/records/CS/input.c-1GAVGMEKRGFCS

echo "clang index tests passed"

popd >/dev/null
