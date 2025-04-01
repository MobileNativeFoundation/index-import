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

echo "Testing import only"
pushd "$base_dir"/import_only >/dev/null

# Clean any test state from previous runs.
rm -fr input output

# Produce the index.
clang -fsyntax-only -index-store-path input input.c "-ffile-prefix-map=$PWD=."

"$index_import" input output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/* \
  | FileCheck "-DPWD=$PWD" expected.txt

# Check that the expected index files exist.
ls output/v5/units/input.c.o-* >/dev/null
ls output/v5/records/MX/input.c-1N81D6PPYGQMX >/dev/null

# Check that the record files are identical.
diff -q -r {input,output}/v5/records/

echo "import only tests passed"
popd >/dev/null

############################################################

echo "Testing import-output-file"
pushd "$base_dir"/import_only >/dev/null

# Clean any test state from previous runs.
rm -fr input output

# Produce the index.
clang -fsyntax-only -index-store-path input input.c

"$index_import" \
  -import-output-file input.c.o \
  input output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/input.c.o-* \
  | FileCheck "-DPWD=$PWD" expected.txt

# Check that the expected index files exist.
ls output/v5/units/input.c.o-* >/dev/null
ls output/v5/records/MX/input.c-1N81D6PPYGQMX >/dev/null

# Check that the record files are identical.
diff -q -r {input,output}/v5/records/

echo "import-output-file tests passed"
popd >/dev/null

###########################################################

echo "Testing import-output-file with -file-prefix-map"
pushd "$base_dir"/import_only >/dev/null

# Clean any test state from previous runs.
rm -fr input output

# Produce the index.
clang -fsyntax-only -index-store-path input input.c "-ffile-prefix-map=$PWD=."

"$index_import" \
  -import-output-file input.c.o \
  -file-prefix-map "$PWD=." \
  input output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/input.c.o-* \
  | FileCheck "-DPWD=." expected.txt

# Check that the expected index files exist.
ls output/v5/units/input.c.o-* >/dev/null
ls output/v5/records/MX/input.c-1N81D6PPYGQMX >/dev/null

# Check that the record files are identical.
diff -q -r {input,output}/v5/records/

echo "import-output-file with -file-prefix-map tests passed"
popd >/dev/null

############################################################

echo "Testing clang indexes"
pushd "$base_dir"/clang >/dev/null

# Clean any test state from previous runs.
rm -fr input output

# Produce the index.
clang -fsyntax-only -index-store-path input input.c

"$index_import" \
  -remap "$PWD/input.c.o"="/fake/working/dir/output.c.o" \
  -remap "$PWD=/fake/working/dir" \
  input output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/* \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output.c.o-2LQD3ZSM9CGHD >/dev/null
ls output/v5/records/MX/input.c-1N81D6PPYGQMX >/dev/null

# Check that the record files are identical.
diff -q -r {input,output}/v5/records/

echo "clang index tests passed"
popd >/dev/null

############################################################

echo "Testing clang indexes with remapping"
pushd "$base_dir"/clang >/dev/null

# Clean any test state from previous runs.
rm -fr input output

# Produce the index.
clang -fsyntax-only -index-store-path input input.c "-ffile-prefix-map=$PWD=."

"$index_import" \
  -remap '\./input.c.o=output.c.o' \
  -remap '^\.=/fake/working/dir' \
  input output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/* \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output.c.o-2LQD3ZSM9CGHD >/dev/null
ls output/v5/records/MX/input.c-1N81D6PPYGQMX >/dev/null

# Check that the record files are identical.
diff -q -r {input,output}/v5/records/

echo "clang index tests with remapping passed"
popd >/dev/null

############################################################

echo "Testing clang indexes with explicit unit output path"
pushd "$base_dir"/clang >/dev/null

# Clean any test state from previous runs.
rm -fr input output

# Produce the index.
clang -fsyntax-only -index-store-path input input.c -index-unit-output-path /foo/input.c.o

"$index_import" \
  -remap '/foo/input.c.o=/fake/working/dir/output.c.o' \
  -remap "$PWD=/fake/working/dir" \
  input output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/* \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output.c.o-2LQD3ZSM9CGHD >/dev/null
ls output/v5/records/MX/input.c-1N81D6PPYGQMX >/dev/null

# Check that the record files are identical.
diff -q -r {input,output}/v5/records/

echo "clang index tests with explicit unit output path passed"
popd >/dev/null

############################################################

echo "Testing swiftc indexes"
pushd "$base_dir"/swiftc >/dev/null

# Clean any test state from previous runs.
rm -fr input output

# Produce the index and delete the unneeded .o.
xcrun swiftc -target "$(uname -m)-apple-macosx10.9.0" -index-store-path input -c input.swift -file-prefix-map "$PWD=." && rm input.o

"$index_import" \
  -remap '\./input.o=output.o' \
  -remap "^\.=/fake/working/dir" \
  input output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/output* output/v5/units/*.swiftinterface* \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output.o-2L127TAXYGI6T >/dev/null
ls output/v5/records/S9/input.swift-1M4LGH2SWM0S9 >/dev/null

# Check that the record files are identical.
diff -q -r {input,output}/v5/records/

echo "swiftc index tests passed"
popd >/dev/null

############################################################

echo "Testing swiftc indexes explicit unit output path"
pushd "$base_dir"/swiftc >/dev/null

# Clean any test state from previous runs.
rm -fr input output

# Produce the index and delete the unneeded .o.
xcrun swiftc -target "$(uname -m)-apple-macosx10.9.0" -index-store-path input -c input.swift -index-unit-output-path /foo/someoutput.o && rm input.o

"$index_import" \
  -remap '/foo/someoutput.o=output.o' \
  -remap "$PWD=/fake/working/dir" \
  input output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/output* output/v5/units/*.swiftinterface* \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output.o-2L127TAXYGI6T >/dev/null
ls output/v5/records/S9/input.swift-1M4LGH2SWM0S9 >/dev/null

# Check that the record files are identical.
diff -q -r {input,output}/v5/records/

echo "swiftc index tests explicit unit output path passed"
popd >/dev/null

############################################################

echo "Testing multiple indexes"
pushd "$base_dir"/multiple >/dev/null

# Clean any test state from previous runs.
rm -fr input1 input2 output

# Produce the two indexes.
clang -fsyntax-only -index-store-path input1 input1.c "-ffile-prefix-map=$PWD=."
clang -fsyntax-only -index-store-path input2 input2.c "-ffile-prefix-map=$PWD=."

"$index_import" \
  -parallel-stride 1 \
  -remap '^\./input(.).c.o=output$1.c.o' \
  -remap '^\.=/fake/working/dir' \
  input1 input2 output

# See https://llvm.org/docs/CommandGuide/FileCheck.html
"$absolute_unit" \
  output/v5/units/* \
  | FileCheck expected.txt

# Check that the expected index files exist.
ls output/v5/units/output1.c.o-383YT9Q6Q1VBR >/dev/null
ls output/v5/units/output2.c.o-3OMGQ7MOFBSUX >/dev/null
ls output/v5/records/L5/input1.c-3D4JIVRT3MUL5 >/dev/null
ls output/v5/records/FG/input2.c-V47TGXUYI0FG >/dev/null

# Check that the record files are identical.
for record in {input1,input2}/v5/records/*; do
    diff -q -r "$record" output/v5/records/"$(basename "$record")"
done

echo "Multiple indexes tests passed"
popd >/dev/null
