cmake_minimum_required(VERSION 3.13)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(CMAKE_CXX_STANDARD 17)

if(COMMAND project)
  project(index-import)
endif()

find_package(re2 QUIET)
if(NOT re2_FOUND)
  # There's no re2 library available on this system, download and
  # build it from GitHub.
  
  # Abseil is a build requirement for re2.
  set(ABSL_PROPAGATE_CXX_STD ON)
  set(ABSL_ENABLE_INSTALL ON)
  include(FetchContent)
  FetchContent_Declare(
    absl
    GIT_REPOSITORY "https://github.com/abseil/abseil-cpp.git"
    GIT_TAG "20230125.3"
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


find_package(Clang CONFIG REQUIRED)

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
