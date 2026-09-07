cmake_minimum_required(VERSION 3.22)
project(trading_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release build optimizations
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -march=native -g -DNDEBUG")

# Common flags
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -fno-exceptions          # latency: no exception overhead
    -fno-rtti               # latency: no RTTI overhead
)

# Thread sanitizer support
option(TSAN "Enable ThreadSanitizer" OFF)
if(TSAN)
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

option(ASAN "Enable AddressSanitizer" OFF)
if(ASAN)
    add_compile_options(-fsanitize=address -g -O1)
    add_link_options(-fsanitize=address)
endif()

# Undefined-behavior sanitizer support
option(UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
if(UBSAN)
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined -g -O1)
    add_link_options(-fsanitize=undefined)
endif()

find_package(Threads REQUIRED)

# ── Core library ─────────────────────────────────────────────────────────────
add_library(engine_core STATIC
    src/core/order_book.cpp
    src/core/matching_engine.cpp
    src/core/market_data.cpp
    src/core/execution_layer.cpp
    src/util/logger.cpp
    src/util/allocator.cpp
)

target_include_directories(engine_core PUBLIC include)
target_link_libraries(engine_core PUBLIC Threads::Threads)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(trading_engine src/main.cpp)
target_link_libraries(trading_engine PRIVATE engine_core)

# ── Benchmarks ────────────────────────────────────────────────────────────────
add_executable(bench_latency bench/bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE engine_core)

add_executable(bench_throughput bench/bench_throughput.cpp)
target_link_libraries(bench_throughput PRIVATE engine_core)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE engine_core)
add_test(NAME OrderBook COMMAND test_order_book)

# Fast unit tests — in ctest, <1ms
add_executable(test_spsc_unit tests/test_spsc_unit.cpp)
target_link_libraries(test_spsc_unit PRIVATE engine_core)
add_test(NAME SPSCUnit COMMAND test_spsc_unit)

# Stress tests — NOT in ctest, run manually (~30s)
add_executable(test_spsc_stress tests/test_spsc.cpp)
target_link_libraries(test_spsc_stress PRIVATE engine_core)

add_executable(test_matching tests/test_matching.cpp)
target_link_libraries(test_matching PRIVATE engine_core)
add_test(NAME Matching COMMAND test_matching)

add_executable(test_allocator tests/test_allocator.cpp)
target_link_libraries(test_allocator PRIVATE engine_core)
add_test(NAME Allocator COMMAND test_allocator)

add_executable(test_mpmc tests/test_mpmc.cpp)
target_link_libraries(test_mpmc PRIVATE engine_core)
add_test(NAME MPMC COMMAND test_mpmc)

add_executable(bench_avx2 bench/bench_avx2.cpp)
target_link_libraries(bench_avx2 PRIVATE engine_core)

add_executable(bench_multisymbol bench/bench_multisymbol.cpp)
target_link_libraries(bench_multisymbol PRIVATE engine_core)

add_executable(test_multi_symbol_engine tests/test_multi_symbol_engine.cpp)
target_link_libraries(test_multi_symbol_engine PRIVATE engine_core)
add_test(NAME MultiSymbolEngine COMMAND test_multi_symbol_engine)

add_executable(test_replay tests/test_replay.cpp)
target_link_libraries(test_replay PRIVATE engine_core)
add_test(NAME Replay COMMAND test_replay)

add_executable(test_market_data tests/test_market_data.cpp)
target_link_libraries(test_market_data PRIVATE engine_core)
add_test(NAME MarketData COMMAND test_market_data)

add_executable(feed_to_execution_demo examples/feed_to_execution_demo.cpp)
target_link_libraries(feed_to_execution_demo PRIVATE engine_core)
add_test(NAME FeedToExecutionDemo COMMAND feed_to_execution_demo)

add_executable(recovery_demo examples/recovery_demo.cpp)
target_link_libraries(recovery_demo PRIVATE engine_core)
add_test(NAME RecoveryDemo COMMAND recovery_demo)

add_executable(test_conservation tests/test_conservation.cpp)
target_link_libraries(test_conservation PRIVATE engine_core)
# Multiple seeds at 1M events each in CI (see LIMITATIONS.md / README
# Correctness section for what's checked after every event). A single
# seed can miss an ordering-dependent bug — see bug #4 in the README,
# which a single fixed seed happened not to reach.
add_test(NAME Conservation-Seed1 COMMAND test_conservation 1 1000000)
add_test(NAME Conservation-Seed2 COMMAND test_conservation 2 1000000)
add_test(NAME Conservation-Seed3 COMMAND test_conservation 3 1000000)
add_test(NAME Conservation-Seed4 COMMAND test_conservation 4 1000000)
add_test(NAME Conservation-Seed5 COMMAND test_conservation 5 1000000)
set_tests_properties(
    Conservation-Seed1 Conservation-Seed2 Conservation-Seed3
    Conservation-Seed4 Conservation-Seed5
    PROPERTIES TIMEOUT 120
)
# A longer (10M-event) run is not part of the default ctest pass — same
# binary, invoked with a bigger event count and a longer timeout from a
# dedicated CI job (see .github/workflows/ci.yml: conservation-long).

add_executable(replay_trace tools/replay_trace.cpp)
target_link_libraries(replay_trace PRIVATE engine_core)

add_executable(soak_test tools/soak_test.cpp)
target_link_libraries(soak_test PRIVATE engine_core)
add_test(NAME SoakTestSmoke COMMAND soak_test 15 5)
set_tests_properties(SoakTestSmoke PROPERTIES TIMEOUT 60)
# Record once at configure-adjacent time isn't right for CMake, so this is
# a ctest step: record a trace, then replay it in a second process and
# byte-diff the fills — proves determinism (same input -> same output)
# independent of test_conservation.cpp's correctness-against-a-model claim.
add_test(NAME ReplayDeterminism
         COMMAND ${CMAKE_COMMAND}
                 -DREPLAY_BIN=$<TARGET_FILE:replay_trace>
                 -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_replay_determinism.cmake)

add_executable(test_histogram tests/test_histogram.cpp)
target_link_libraries(test_histogram PRIVATE engine_core)
add_test(NAME Histogram COMMAND test_histogram)

add_executable(bench_cache bench/bench_cache.cpp)
target_link_libraries(bench_cache PRIVATE engine_core)

add_executable(bench_replay bench/bench_replay.cpp)
target_link_libraries(bench_replay PRIVATE engine_core)
