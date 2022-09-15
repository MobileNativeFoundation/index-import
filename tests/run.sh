#!/bin/bash

# shellcheck disable=SC2016
set -euo pipefail

base_dir=$(dirname "$0")
readonly index_import=../../build/index-import
readonly absolute_unit=../../build/absolute-unit

clang() {
    xcrun --sdk macosx clang -mmacosx-version-min=10.0.0 "$@"
}

############################################################

echo "Testing clang indexes"
pushd "$base_dir"/clang >/dev/null

# Clean any test state from previous runs.
rm -fr input output

# Produce the index.
clang -fsyntax-only -index-store-path input input.c

"$index_import" \
  -remap input.c.o=output.c.o \
  -remap "$(pwd)"=/fake/working/dir \
  input output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/* \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output.c.o-1I92L511L7IRP >/dev/null
ls output/v5/records/2F/input.c-50XRP2AC092F >/dev/null

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
xcrun swiftc -target "$(uname -m)-apple-macosx10.9.0" -index-store-path input -c input.swift && rm input.o

"$index_import" \
  -remap input.o=output.o \
  -remap "$(pwd)"=/fake/working/dir \
  input output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/output* output/v5/units/*.swiftinterface* \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output.o-2WR4IG6X35AJB >/dev/null
ls output/v5/records/XT/input.swift-2PHAH6J9HFCXT >/dev/null

# Check that the record files are identical.
diff -q -r {input,output}/v5/records/

echo "swiftc index tests passed"
popd >/dev/null

############################################################

echo "Testing multiple indexes"
pushd "$base_dir"/multiple >/dev/null

# Clean any test state from previous runs.
rm -fr input1 input2 output

# Produce the two indexes.
clang -fsyntax-only -index-store-path input1 input1.c
clang -fsyntax-only -index-store-path input2 input2.c

"$index_import" \
  -parallel-stride 1 \
  -remap 'input(.).c.o=output$1.c.o' \
  -remap "$(pwd)"=/fake/working/dir \
  input1 input2 output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/* \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output1.c.o-ZW8ISK3OCQ8L >/dev/null
ls output/v5/units/output2.c.o-1ZBCL54RNWOPC >/dev/null
ls output/v5/records/WU/input1.c-H8E66JWPU5WU >/dev/null
ls output/v5/records/LF/input2.c-1UNY7PC9RPELF >/dev/null

# Check that the record files are identical.
for record in {input1,input2}/v5/records/*; do
    diff -q -r "$record" output/v5/records/"$(basename "$record")"
done

echo "Multiple indexes tests passed"
popd >/dev/null
