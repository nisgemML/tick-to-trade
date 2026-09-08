cmake_minimum_required(VERSION 3.22)
project(hft_stack VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)
set(OE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/options-engine)
set(UDP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/udp-multicast-receiver)
add_library(engine_core STATIC
    ${OE_ROOT}/src/core/order_book.cpp
    ${OE_ROOT}/src/core/matching_engine.cpp
    ${OE_ROOT}/src/core/market_data.cpp
    ${OE_ROOT}/src/core/execution_layer.cpp
    ${OE_ROOT}/src/util/logger.cpp
    ${OE_ROOT}/src/util/allocator.cpp
)
target_include_directories(engine_core PUBLIC ${OE_ROOT}/include)
target_link_libraries(engine_core PUBLIC Threads::Threads)
add_library(hft_iface INTERFACE)
target_include_directories(hft_iface INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OE_ROOT}/include
    ${UDP_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpsc-queue/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/io-uring-queue/include
)
target_link_libraries(hft_iface INTERFACE engine_core Threads::Threads)
add_executable(run_pipeline tools/run_pipeline.cpp)
target_link_libraries(run_pipeline PRIVATE hft_iface)
add_executable(run_feed_pipeline tools/run_feed_pipeline.cpp)
target_link_libraries(run_feed_pipeline PRIVATE hft_iface)
add_executable(bench_pipeline bench/bench_pipeline.cpp)
target_link_libraries(bench_pipeline PRIVATE hft_iface)
add_executable(bench_tick_to_trade bench/bench_tick_to_trade.cpp)
target_link_libraries(bench_tick_to_trade PRIVATE hft_iface)
enable_testing()
add_executable(test_pipeline_smoke tests/test_pipeline_smoke.cpp)
target_link_libraries(test_pipeline_smoke PRIVATE hft_iface)
add_test(NAME pipeline_smoke COMMAND test_pipeline_smoke)
add_executable(test_backpressure tests/test_backpressure.cpp)
target_link_libraries(test_backpressure PRIVATE hft_iface)
add_test(NAME backpressure COMMAND test_backpressure)
add_executable(test_conservation tests/test_conservation.cpp)
target_link_libraries(test_conservation PRIVATE hft_iface)
add_test(NAME conservation COMMAND test_conservation)
add_executable(test_feed_adapter tests/test_feed_adapter.cpp)
target_link_libraries(test_feed_adapter PRIVATE hft_iface)
add_test(NAME feed_adapter COMMAND test_feed_adapter)
