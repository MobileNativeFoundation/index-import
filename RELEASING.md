To build and release a multi architecture executable you need to follow
a few steps:

1. Prepare the Swift build for both Intel and Apple Silicon. This
   command varies a bit depending on your host architecture. For your
   host architecture run:

```sh
./swift/utils/build-script --release --skip-build --llvm-targets-to-build [X86|ARM]
ninja -C build/Ninja-ReleaseAssert/llvm-macosx-$(arch) libIndexStore.dylib
```

For the architecture you need to cross compile:

```sh
./swift/utils/build-script --release --skip-build --llvm-targets-to-build [X86|ARM] --cross-compile-hosts macosx-[x86_64|arm64]
ninja -C build/Ninja-ReleaseAssert/llvm-macosx-[x86_64|arm64] libIndexStore.dylib
```

2. Build index-import for both architectures:

```sh
mkdir intelbuild
cd intelbuild
PATH="path/to/swift-source/build/Ninja-ReleaseAssert/llvm-macosx-x86_64/bin:$PATH"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. -DCMAKE_OSX_ARCHITECTURES=x86_64
ninja
strip -STx index-import absolute-unit
```

```sh
mkdir armbuild
cd armbuild
PATH="path/to/swift-source/build/Ninja-ReleaseAssert/llvm-macosx-arm64/bin:$PATH"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. -DCMAKE_OSX_ARCHITECTURES=arm64
ninja
strip -STx index-import absolute-unit
```

3. Combine the executables we want to release:

```sh
lipo -create -output absolute-unit intelbuild/absolute-unit armbuild/absolute-unit
lipo -create -output index-import intelbuild/index-import armbuild/index-import
```

4. Create an archive:

```sh
tar czvf index-import.tar.gz absolute-unit index-import
```
