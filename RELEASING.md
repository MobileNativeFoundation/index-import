To build and release a multi architecture executable you need to follow
a few steps:

1. Build Apple's LLVM fork for both Intel and Apple Silicon. Currently
   doing this with LLVM's cmake configuration is a bit easier than using
   Swift's build-script, but that might change in the future:

```sh
cd /path/to/apple/llvm-project
mkdir build
cd build
cmake ../llvm -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" -DLLVM_ENABLE_PROJECTS=clang
ninja libIndexStore.dylib
```

2. Build index-import for both architectures:

```sh
cd /path/to/index-import
mkdir build
PATH="/path/to/apple/llvm-project/build/bin:$PATH"
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
ninja
strip -STx index-import absolute-unit
```

3. Create an archive:

```sh
COPYFILE_DISABLE=1 tar czvf index-import.tar.gz absolute-unit index-import
```
