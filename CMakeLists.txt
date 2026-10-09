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

mercury_executable(hello src/main.cpp)
mercury_executable(ping src/ping.cpp)
mercury_executable(hello_server src/server.cpp)
target_link_libraries(hello_server PRIVATE mercury_http)
mercury_executable(mercury src/mercury.cpp src/daemons.cpp src/counters.cpp src/ring.cpp src/ring_archive.cpp)
target_link_libraries(mercury PRIVATE mercury_http Threads::Threads)
mercury_executable(perfcount src/perfcount.cpp)
mercury_executable(daemon_test src/test/main.cpp)
target_compile_definitions(daemon_test PRIVATE DAEMONS_TXT="${CMAKE_SOURCE_DIR}/src/test/daemons.txt")
