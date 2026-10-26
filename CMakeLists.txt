cmake_minimum_required(VERSION 3.16)
project(mercury LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(mercury_http STATIC src/http.cpp)
target_include_directories(mercury_http PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(mercury_http PRIVATE -Wall -Wextra -Wpedantic -march=native)

function(mercury_executable name)
  add_executable(${name} ${ARGN})
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -march=native)
endfunction()

function(mercury_test name)
  mercury_executable(${name} ${ARGN})
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/test")
endfunction()

mercury_executable(mercury src/mercury.cpp src/daemons.cpp src/counters.cpp src/ring.cpp src/ring_archive.cpp src/policy.cpp src/resources.cpp src/threadstat.cpp src/gnn.cpp src/gru.cpp src/cpustat.cpp)
mercury_executable(speculator src/speculator.cpp)
target_link_libraries(mercury PRIVATE mercury_http Threads::Threads)
mercury_executable(perfcount src/perfcount.cpp)
mercury_test(env_probe src/test/env_probe.cpp)
mercury_test(resources_test src/test/resources.cpp src/resources.cpp src/ring.cpp src/ring_archive.cpp src/policy.cpp)
target_include_directories(resources_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(resources_test PRIVATE Threads::Threads)
mercury_test(ring_test src/test/ring.cpp src/ring.cpp src/ring_archive.cpp)
target_include_directories(ring_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
mercury_test(cpustat_test src/test/cpustat.cpp src/cpustat.cpp src/counters.cpp src/ring.cpp src/ring_archive.cpp)
target_include_directories(cpustat_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(cpustat_test PRIVATE Threads::Threads)
mercury_test(policy_test src/test/policy.cpp src/policy.cpp)
target_include_directories(policy_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(policy_test PRIVATE TRACES_DIR="${CMAKE_SOURCE_DIR}/src/test/traces")
mercury_test(spinner src/test/spinner.cpp)
target_link_libraries(spinner PRIVATE Threads::Threads)
# gnn policy phase 1 (SPEC.md): per-thread trace recorder and synthetic workloads.
mercury_executable(threadtrace src/threadtrace.cpp src/threadstat.cpp src/counters.cpp src/ring.cpp src/ring_archive.cpp)
target_link_libraries(threadtrace PRIVATE Threads::Threads)
# The gnn controller runs every 10 ms inside mercury: optimise its sources even in the default (unoptimised) build.
set_source_files_properties(src/gnn.cpp src/threadstat.cpp PROPERTIES COMPILE_OPTIONS "-O2")
# The gru autoscaler's matrix-vector products: -O3 vectorizes them (no -ffast-math: results stay bit-for-bit).
set_source_files_properties(src/gru.cpp PROPERTIES COMPILE_OPTIONS "-O3")
mercury_executable(gnnreplay src/gnnreplay.cpp src/gnn.cpp src/gru.cpp src/threadstat.cpp src/counters.cpp src/ring.cpp src/ring_archive.cpp)
mercury_test(gru_test src/test/gru.cpp src/gnn.cpp src/gru.cpp src/threadstat.cpp src/counters.cpp src/ring.cpp src/ring_archive.cpp)
target_include_directories(gru_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(gru_test PRIVATE MERCURY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
mercury_test(gnnload src/test/workloads/gnnload.cpp)
target_link_libraries(gnnload PRIVATE Threads::Threads)

# ctest: the saturate decisions, the placement walk, the ring archive and the per-CPU parsers (no root, any host), and the route smoke test
# (root, a few seconds). saturate_integration is the slow end-to-end check on dtn-01 (NUMA nodes 0-2,
# about a minute, known flaky); it carries the label "slow": `ctest -LE slow` skips it.
# The shell tests re-execute themselves with sudo -n for cgroup writes.
enable_testing()
add_test(NAME policy_test COMMAND policy_test)
add_test(NAME gru_test COMMAND gru_test)
add_test(NAME resources_test COMMAND resources_test)
add_test(NAME ring_test COMMAND ring_test)
add_test(NAME cpustat_test COMMAND cpustat_test)
add_test(NAME smoke_integration COMMAND ${CMAKE_SOURCE_DIR}/src/test/smoke_it.sh)
set_tests_properties(smoke_integration PROPERTIES
  ENVIRONMENT "MERCURY_BIN=$<TARGET_FILE:mercury>;PROBE_BIN=$<TARGET_FILE:env_probe>" TIMEOUT 60)
add_test(NAME saturate_integration COMMAND ${CMAKE_SOURCE_DIR}/src/test/saturate_it.sh)
set_tests_properties(saturate_integration PROPERTIES
  ENVIRONMENT "MERCURY_BIN=$<TARGET_FILE:mercury>;SPIN_BIN=$<TARGET_FILE:spinner>" TIMEOUT 600 LABELS slow)
