cmake_minimum_required(VERSION 3.21)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LOB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(LOB_BUILD_TESTS "Build unit and differential tests" ON)
option(LOB_BUILD_BENCH "Build benchmarks" OFF)

# The engine is header-only: an INTERFACE target carries the include path and flags.
add_library(lob INTERFACE
        include/lob/types.h
        include/lob/commands.h
        include/lob/baseline.h
        include/lob/concepts.h
        include/lob/events.h
        include/lob/policies.h
        include/lob/fast_book.h
)
add_library(lob::lob ALIAS lob)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob INTERFACE cxx_std_20)

# Warnings for everything we compile ourselves (not for fetched dependencies).
add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Werror)

if(LOB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

# Use archive timestamps as extraction time (CMake 3.24+); silences a warning without breaking 3.21-3.23.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)

if(LOB_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_subdirectory(tests)
endif()

# The latency benchmark times each operation itself (rdtsc), so it needs no benchmark library.
if(LOB_BUILD_BENCH)
  add_subdirectory(bench)
endif()
