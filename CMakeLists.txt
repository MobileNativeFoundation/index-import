cmake_minimum_required(VERSION 3.28)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(CMAKE_CXX_STANDARD 17)

if(COMMAND project)
  project(index-import)
endif()

include(FetchContent)
option(INDEX_IMPORT_BUILD_LLVM "Build Swift's LLVM fork instead of using an existing Clang package" ON)
set(INDEX_IMPORT_LLVM_REVISION "swift-6.4.0-RELEASE"
    CACHE STRING "Swift LLVM commit SHA or tag to build")

if(INDEX_IMPORT_BUILD_LLVM)
  set(LLVM_ENABLE_PROJECTS clang CACHE STRING "LLVM projects to build")
  set(LLVM_TARGETS_TO_BUILD "X86;AArch64" CACHE STRING "LLVM targets to build")
  set(LLVM_PARALLEL_LINK_JOBS 2 CACHE STRING "Maximum parallel LLVM link jobs")
  set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "Build LLVM tests")
  set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "Build LLVM benchmarks")
  set(LLVM_USE_STATIC_ZSTD ON CACHE BOOL "Link zstd statically")
  FetchContent_Declare(
    swift_llvm
    URL "https://github.com/swiftlang/llvm-project/archive/${INDEX_IMPORT_LLVM_REVISION}.tar.gz"
    SOURCE_SUBDIR llvm
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(swift_llvm)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/llvm-revision.txt"
       CONTENT "${INDEX_IMPORT_LLVM_REVISION}\n")
  set(LLVM_INCLUDE_DIRS "${swift_llvm_SOURCE_DIR}/llvm/include" "${swift_llvm_BINARY_DIR}/include")
  set(CLANG_INCLUDE_DIRS "${swift_llvm_SOURCE_DIR}/clang/include" "${swift_llvm_BINARY_DIR}/tools/clang/include")
else()
  find_package(Clang CONFIG REQUIRED)
endif()

find_package(re2 QUIET)
if(NOT re2_FOUND)
  # There's no re2 library available on this system, download and
  # build it from GitHub.

  # Abseil is a build requirement for re2.
  set(ABSL_PROPAGATE_CXX_STD ON)
  set(ABSL_ENABLE_INSTALL ON)
  FetchContent_Declare(
    absl
    GIT_REPOSITORY "https://github.com/abseil/abseil-cpp.git"
    GIT_TAG "20250127.1"
  )
  FetchContent_MakeAvailable(absl)

  # Download and build a known-good version of re2.
  FetchContent_Declare(
    re2
    GIT_REPOSITORY "https://github.com/google/re2.git"
    GIT_TAG "2023-06-02"
  )
  FetchContent_MakeAvailable(re2)
endif()

function(add_index_executable execname)
  add_executable("${execname}" "${execname}.cpp")
  target_include_directories("${execname}" SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  target_compile_features("${execname}" PRIVATE cxx_std_17)
  set_target_properties("${execname}" PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_options("${execname}" PRIVATE -Wall -Wextra -fblocks)
  if(NOT LLVM_ENABLE_RTTI)
    target_compile_options("${execname}" PRIVATE -fno-rtti)
  endif()
  target_link_libraries("${execname}" PRIVATE clangIndexDataStore re2::re2)

  if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    target_link_options("${execname}" PRIVATE -dead_strip)
  endif()
endfunction()

add_index_executable(index-import)
add_index_executable(absolute-unit)
add_index_executable(validate-index)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_program(SWIFTC_EXECUTABLE swiftc)
  if(SWIFTC_EXECUTABLE)
    get_filename_component(SWIFTC_REALPATH "${SWIFTC_EXECUTABLE}" REALPATH)
    get_filename_component(SWIFT_BIN_DIR "${SWIFTC_REALPATH}" DIRECTORY)
  endif()
  find_path(DISPATCH_INCLUDE_DIR dispatch/dispatch.h
    HINTS "${SWIFT_BIN_DIR}/../lib/swift")
  find_library(DISPATCH_LIBRARY dispatch
    HINTS "${SWIFT_BIN_DIR}/../lib/swift_static/linux" "${SWIFT_BIN_DIR}/../lib/swift/linux")
  find_library(BLOCKS_RUNTIME_LIBRARY BlocksRuntime
    HINTS "${SWIFT_BIN_DIR}/../lib/swift_static/linux" "${SWIFT_BIN_DIR}/../lib/swift/linux")
  if(NOT DISPATCH_INCLUDE_DIR OR NOT DISPATCH_LIBRARY OR NOT BLOCKS_RUNTIME_LIBRARY)
    message(FATAL_ERROR "libdispatch headers, libdispatch, and BlocksRuntime are required on Linux")
  endif()
  target_include_directories(index-import SYSTEM PRIVATE "${DISPATCH_INCLUDE_DIR}")
  target_link_libraries(index-import PRIVATE "${DISPATCH_LIBRARY}" "${BLOCKS_RUNTIME_LIBRARY}")
endif()

if(INDEX_IMPORT_BUILD_LLVM)
  add_custom_target(check-index-import
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh"
    DEPENDS index-import absolute-unit FileCheck validate-index
    USES_TERMINAL
  )
endif()
