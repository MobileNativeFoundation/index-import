#!/bin/bash

set -euo pipefail

readonly base_dir=$(dirname "$0")

############################################################

echo "Testing clang indexes"
pushd "$base_dir"/clang >/dev/null

# Clean any test state from previous runs.
rm -fr input output

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
diff -q -r {input,output}/v5/records/

echo "clang index tests passed"
popd >/dev/null

############################################################

echo "Testing swiftc indexes"
pushd "$base_dir"/swiftc >/dev/null

# Clean any test state from previous runs.
rm -fr input output

# Produce the index and delete the unneeded .o.
xcrun swiftc -index-store-path input -c input.swift && rm input.o

# Test index-import by matching its verbose output.
# See https://llvm.org/docs/CommandGuide/FileCheck.html
../../build/index-import -V \
  -remap input.o=output.o \
  -remap "$(pwd)"=/fake/working/dir \
  input output \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output.o-2WR4IG6X35AJB >/dev/null
ls output/v5/records/XT/input.swift-2PHAH6J9HFCXT >/dev/null

# Check that the record files are identical.
diff -q -r {input,output}/v5/records/

echo "swiftc index tests passed"
popd >/dev/null
