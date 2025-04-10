# index-import

A tool to import `swiftc` and `clang` generated indexes into Xcode.

## Overview

The `index-import` tool makes indexes portable. The ability to copy indexes into Xcode has a few possible uses:

1. Using a separate build system (Bazel, Buck, CMake, SwiftPM, etc)
2. Distributing a CI built index to developer machines
3. Sharing an index between two or more local checkouts

The common goal is to reduce or eliminate the time Xcode spends indexing.

For Xcode to be able to use an index, the file paths contained in the index must match the paths Xcode uses for lookup. This is the main feature of `index-import`, rewriting the paths inside the index files. This path remapping requires knowing input and output paths.

Path remapping is done with regex substitution. `index-import` accepts one or more `-remap` flags which are formatted as `<regex>=<substitution>`. See the [examples](#examples) below. Path remapping is conceptually similar to `sed s/regex/substitution/`. In all cases, the substitution will either be a path within the project, or a path within `DerivedData`.

## Examples

The simplest example is to consider the case of two checkouts of the same project on the same machine. If one project has a built index, it can be imported into the other. To do this, two paths need to be remapped: the project directory and the build directory (`DerivedData`).

```sh
#!/bin/bash

build_dir1="/Users/me/Library/Developer/Xcode/DerivedData/MyApp-abc123"
build_dir2="/Users/me/Library/Developer/Xcode/DerivedData/MyApp-xyz789"

index-import \
    -remap "/Users/me/MyApp=/Users/me/MyApp2" \
    -remap "$build_dir1=$build_dir2" \
    "$build_dir1/Index/DataStore" \
    "$build_dir2/Index/DataStore"
```

A more complex example is importing an index from a [Bazel](https://bazel.build) built project. This example would be run as an Xcode "Run Script" build phase, which provides many environment variables, including: `SRCROOT`, `CONFIGURATION_TEMP_DIR`, `ARCHS`.

```sh
#!/bin/bash

set -euo pipefail

# Input: /Users/me/Library/Developer/Xcode/DerivedData/PROJECT-abc/Build/Products
# Output: /Users/me/Library/Developer/Xcode/DerivedData/PROJECT-abc
derived_data_root=$(dirname "$(dirname "$BUILD_DIR")")
readonly xcode_index_root="$derived_data_root/Index.noindex/DataStore"

# Captures: 1) module name
readonly bazel_swiftmodules="^/__build_bazel_rules_swift/swiftmodules/(.+).swiftmodule"
readonly xcode_swiftmodules="$BUILT_PRODUCTS_DIR/\$1.swiftmodule/$ARCHS.swiftmodule"

# Captures: 1) target name, 2) object name
readonly bazel_objects="^\./bazel-out/.+?/bin/.*?(?:[^/]+)/([^/]+?)_objs(?:/.*)*/(.+?)\.swift\.o$"
readonly xcode_objects="$CONFIGURATION_TEMP_DIR/\$1.build/Objects-normal/$ARCHS/\$2.o"

index-import
    -remap "$bazel_swiftmodules=$xcode_swiftmodules" \
    -remap "$bazel_objects=$xcode_objects" \
    -remap "^\.=$SRCROOT" \
    -remap "DEVELOPER_DIR=$DEVELOPER_DIR" \
    -incremental \
    @"$index_stores_file" \
    "$xcode_index_root"
```

Since Xcode 14 / Swift 5.7, `clang` and `swiftc` support remapping paths
in index data using `-ffile-prefix-map=foo=bar` and `-file-prefix-map
foo=bar` respectively. Using this makes it easy to generate a
reproducible index that can be transferred between machines, and then
remapped to local only paths using one of the examples above.

## Build Instructions

The build uses [CMake](https://cmake.org) because [Apple's LLVM fork](https://github.com/apple/llvm-project) uses CMake. The `index-import` build script is small, but depends on the libraries from LLVM. To build `index-import`, first [install the tools required by Swift](https://github.com/apple/swift/blob/main/docs/HowToGuides/GettingStarted.md#system-requirements), then build swift by following the [Swift build instructions](https://github.com/apple/swift/blob/main/docs/HowToGuides/GettingStarted.md#building-the-project-for-the-first-time).

When building Swift, keep the following in mind:

1. Checkout the desired release branch of Swift using something like `./swift/utils/update-checkout --clone --scheme release/5.7`.
2. Build Swift using `--release`/`-R` for performance

Building all of Swift can take a long time, and most of that isn't needed by `index-import`. A faster way to build `index-import`, is to build only `libIndexStore.dylib`. Here are the commands to do just that:

```sh
./swift/utils/build-script --release --skip-build --llvm-targets-to-build AArch64
ninja -C build/Ninja-ReleaseAssert/llvm-macosx-arm64 libIndexStore.dylib
```

Once swift (or `libIndexStore.dylib`) has been built, `index-import` can be built as follows. The _key_ step is to update your `PATH` variable to include the llvm `bin/` directory (from the swift-source build directory). This ensures CMake can find all necessary build dependencies.

```sh
# From the index-import directory
mkdir build
cd build
PATH="path/to/swift-source/build/Ninja-ReleaseAssert/llvm-macosx-arm64/bin:$PATH"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja
```

If you need to cross compile checkout [RELEASING.md](RELEASING.md)

Or, if you prefer Xcode for building and debugging, you can replace the last 2 lines with the following:

```
cmake -G Xcode -DCMAKE_BUILD_TYPE=Release ..
open index-import.xcodeproj
```

## Index File Format

The index consists of two types of files, Unit files and Record files. Both are [LLVM Bitstream](https://www.llvm.org/docs/BitCodeFormat.html#bitstream-format), a common binary format used by LLVM/Clang/Swift. Record files contain no paths and can be simply copied. Only Unit files contain paths, so only unit files need to be rewritten. A read/write API is available in the `clangIndex` library. `index-import` uses [`IndexUnitReader`](https://github.com/apple/llvm-project/blob/swift/release/5.7/clang/include/clang/Index/IndexUnitReader.h) and [`IndexUnitWriter`](https://github.com/apple/llvm-project/blob/swift/release/5.7/clang/include/clang/Index/IndexUnitWriter.h).

## Resources

The best information on the `swiftc` and `clang` index store comes from these two resources:

* [Adding Index‐While‐Building and Refactoring to Clang](https://www.youtube.com/watch?v=jGJhnIT-D2M), 2017 LLVM Developers Meeting, by Alex Lorenz and Nathan Hawes
* [Indexing While Building whitepaper](https://docs.google.com/document/d/1cH2sTpgSnJZCkZtJl1aY-rzy4uGPcrI-6RrUpdATO2Q/)
