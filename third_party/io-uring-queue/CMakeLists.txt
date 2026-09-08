cmake_minimum_required(VERSION 3.22)
project(io_uring_queue VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

find_library(URING_LIB uring REQUIRED)
find_path(URING_INCLUDE liburing.h REQUIRED)

add_library(ioq_iface INTERFACE)
target_include_directories(ioq_iface INTERFACE include ${URING_INCLUDE})
target_link_libraries(ioq_iface INTERFACE ${URING_LIB} pthread)
target_compile_options(ioq_iface INTERFACE -Wall -Wextra -mavx2)

enable_testing()

add_executable(test_ring tests/test_ring.cpp)
target_link_libraries(test_ring PRIVATE ioq_iface)
add_test(NAME RingBuffer COMMAND test_ring)

add_executable(bench_ring bench/bench_ring.cpp)
target_link_libraries(bench_ring PRIVATE ioq_iface)
# Not in ctest — run manually: ./bench_ring

add_executable(matching_engine_demo examples/matching_engine_demo.cpp)
target_link_libraries(matching_engine_demo PRIVATE ioq_iface)
add_test(NAME MatchingEngineDemo COMMAND matching_engine_demo)

add_executable(bench_batching bench/bench_batching.cpp)
target_link_libraries(bench_batching PRIVATE ioq_iface)
