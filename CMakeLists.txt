cmake_minimum_required(VERSION 3.16)
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# --- Core library: V1 (naive) + V2/V3 (optimized) order books + engine facade ---
add_library(matching_engine_core STATIC
  src/naive/order_book.cpp
  src/optimized/order_book.cpp
  src/engine.cpp
)
target_include_directories(matching_engine_core PUBLIC include)
target_compile_options(matching_engine_core PUBLIC -Wall -Wextra)

# --- Tests: GoogleTest via FetchContent, built -O0 -g for debuggability ---
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(matching_engine_tests
  tests/test_matching.cpp
  tests/test_priorities.cpp
  tests/test_edge_cases.cpp
  tests/test_order_types.cpp
)
target_link_libraries(matching_engine_tests PRIVATE matching_engine_core gtest_main)
target_compile_options(matching_engine_tests PRIVATE -O0 -g)
include(GoogleTest)
gtest_discover_tests(matching_engine_tests)

# --- Benchmarks: Google Benchmark via FetchContent, built -O2 -DNDEBUG ---
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(matching_engine_benchmarks
  benchmark/bench_insert.cpp
  benchmark/bench_match.cpp
  benchmark/bench_cancel.cpp
)
target_link_libraries(matching_engine_benchmarks PRIVATE matching_engine_core benchmark::benchmark)
target_compile_options(matching_engine_benchmarks PRIVATE -O2 -DNDEBUG)
