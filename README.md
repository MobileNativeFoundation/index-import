# index-import

A tool to import `swiftc` and `clang` generated indexes into Xcode.

## Overview

The `index-import` tool makes indexes transportable. The ability to copy indexes into Xcode has a few possible uses:

1. Using a separate build system (Bazel, Buck, CMake, SwiftPM, etc)
2. Distributing a CI built index to developer machines
3. Sharing an index between two or more local checkouts

The common goal is to reduce or eliminate the time Xcode spends indexing.

For Xcode to be able to use an index, the file paths contained in the index must match the paths Xcode uses for lookup. This is the main feature of `index-import`, rewriting the paths inside the index files. This path remapping requires knowing input and output paths.

Path remapping is done with regex substitution. `index-import` accepts one or more `-remap` flags which are formatted as `<regex>=<substitution>`. See the [examples](#examples) below. Path remapping is conceptually similar to `sed s/regex/substitution/`. In all cases, the substitution will either be a path within the project, or a path within `DerivedData`.

### Index File Format

The index consists of two types of files, Unit files and Record files. Both are [LLVM Bitstream](https://www.llvm.org/docs/BitCodeFormat.html#bitstream-format), a common binary format used by LLVM/Clang/Swift. Record files contain no paths and can be simply copied. Only Unit files contain paths, so only unit files need to be rewritten. A read/write API is available in the `clangIndex` library. `index-import` uses [`IndexUnitReader`](https://github.com/apple/swift-clang/blob/swift-5.0-branch/include/clang/Index/IndexUnitReader.h) and [`IndexUnitWriter`](https://github.com/apple/swift-clang/blob/swift-5.0-branch/include/clang/Index/IndexUnitWriter.h).

## Resources

The best information on the `swiftc` and `clang` index store comes from these two resources:

* [Adding Index‐While‐Building and Refactoring to Clang](https://www.youtube.com/watch?v=jGJhnIT-D2M), 2017 LLVM Developers Meeting, by Alex Lorenz and Nathan Hawes
* [Indexing While Building whitepaper](https://docs.google.com/document/d/1cH2sTpgSnJZCkZtJl1aY-rzy4uGPcrI-6RrUpdATO2Q/)

## Examples

The simplest example is to consider the case of two checkouts of the same project. If one project has a built index, it can be imported into the other. To do this, two paths need to be remapped: the project directory and the build directory (`DerivedData`).

```sh
#!/bin/bash

build_dir1="/Users/me/Library/Developer/Xcode/DerivedData/MyApp-abc123"
build_dir2="/Users/me/Library/Developer/Xcode/DerivedData/MyApp-xyz789"

index-import \
    -remap "/Users/me/MyApp=/Users/me/MyApp2" \
    -remap "$build_dir1=$build_dir2" \
    "$build_dir1/Index/DataStore" \
    "$build_dir2/Index/DataStore" \
```

A more complex example is importing an index from a [Bazel](https://bazel.build) built project. This example makes would be run as an Xcode "Run Script" build phase, which provides many environment variables, including: `SRCROOT`, `CONFIGURATION_TEMP_DIR`, `ARCHS`.

```sh
#!/bin/bash

bazel_root="^/private/var/tmp/_bazel_[^/]+/[^/]+/execroot/[^/]+"
bazel_module_dir="bazel-out/[^/]+/bin/Modules/([^/]+)"
xcode_module_dir="$CONFIGURATION_TEMP_DIR/\$1.build/Objects-normal/$ARCHS"

index-import \
    -remap "$bazel_root/$bazel_module_dir/\\1.swiftmodule=$xcode_module_dir/\$1.swiftmodule" \
    -remap "^$bazel_module_dir/.+/([^/]+).swift.o=$xcode_module_dir/\$2.o" \
    -remap "$bazel_root=$SRCROOT" \
    path/to/input/index \
    path/to/xcode/index

```

## Build Instructions

The build uses [CMake](https://cmake.org) because [swift-clang](https://www.github.com/apple/swift-clang) uses CMake. The `index-import` build script is small, but depends on the larger swift-clang project. To build `index-import`, first build swift-clang using the [Swift build instructions](https://github.com/apple/swift#building-swift). Be sure to checkout a release branch of swift, not `master`, for example `swift-5.0-branch`.

Once swift has been built, `index-import` can be built as follows. The key step is to add the llvm `bin` directory from the swift build, to your `PATH`. This ensures CMake can find the build files it needs.

```
mkdir build
cd build
PATH="path/to/swift/build/Ninja-ReleaseAssert/llvm-macosx-x86_64/bin:$PATH"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja
```
