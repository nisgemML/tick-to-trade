cmake_minimum_required(VERSION 3.22)
project(hft_stack VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)

# Real io_uring async logging (see include/hft/log_sink.hpp), not just
# vendored-but-unused third_party/io-uring-queue. Off by default so the
# stack keeps building with zero extra dependencies on any Linux box;
# turn it on explicitly once liburing-dev is available.
option(HFT_WITH_IOURING "Use io_uring (not blocking write()) for the execution-report log sink; requires liburing" OFF)
if(HFT_WITH_IOURING)
    find_library(URING_LIB uring)
    find_path(URING_INCLUDE liburing.h)
    if(NOT URING_LIB OR NOT URING_INCLUDE)
        message(FATAL_ERROR
            "HFT_WITH_IOURING=ON but liburing was not found. "
            "Install it (e.g. 'apt install liburing-dev') or configure with -DHFT_WITH_IOURING=OFF.")
    endif()
endif()

# Real NUMA-aware, explicitly hugepage-backed allocation for the feed
# path's GapBuffer (see include/hft/numa_support.hpp), not just the
# default heap allocation it's always used. Off by default, same reason
# as HFT_WITH_IOURING above: zero extra dependencies on any Linux box
# unless explicitly asked for.
option(HFT_WITH_NUMA "NUMA-aware, hugepage-backed GapBuffer allocation in run_live_pipeline; requires libnuma" OFF)
if(HFT_WITH_NUMA)
    find_library(NUMA_LIB numa)
    find_path(NUMA_INCLUDE numa.h)
    if(NOT NUMA_LIB OR NOT NUMA_INCLUDE)
        message(FATAL_ERROR
            "HFT_WITH_NUMA=ON but libnuma was not found. "
            "Install it (e.g. 'apt install libnuma-dev') or configure with -DHFT_WITH_NUMA=OFF.")
    endif()
endif()
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
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp26-alloc/include
)
target_link_libraries(hft_iface INTERFACE engine_core Threads::Threads)
if(HFT_WITH_IOURING)
    target_compile_definitions(hft_iface INTERFACE HFT_HAVE_IOURING=1)
    target_include_directories(hft_iface INTERFACE ${URING_INCLUDE})
    target_link_libraries(hft_iface INTERFACE ${URING_LIB})
endif()
if(HFT_WITH_NUMA)
    target_compile_definitions(hft_iface INTERFACE HFT_HAVE_NUMA=1)
    target_include_directories(hft_iface INTERFACE ${NUMA_INCLUDE})
    target_link_libraries(hft_iface INTERFACE ${NUMA_LIB})
endif()
add_executable(run_pipeline tools/run_pipeline.cpp)
target_link_libraries(run_pipeline PRIVATE hft_iface)
add_executable(run_feed_pipeline tools/run_feed_pipeline.cpp)
target_link_libraries(run_feed_pipeline PRIVATE hft_iface)
add_executable(run_market_maker_demo tools/run_market_maker_demo.cpp)
target_link_libraries(run_market_maker_demo PRIVATE hft_iface)
add_executable(run_live_pipeline tools/run_live_pipeline.cpp)
target_link_libraries(run_live_pipeline PRIVATE hft_iface)
add_executable(send_live_itch tools/send_live_itch.cpp)
target_link_libraries(send_live_itch PRIVATE hft_iface)
add_executable(bench_pipeline bench/bench_pipeline.cpp)
target_link_libraries(bench_pipeline PRIVATE hft_iface)
add_executable(bench_tick_to_trade bench/bench_tick_to_trade.cpp)
target_link_libraries(bench_tick_to_trade PRIVATE hft_iface)
add_executable(bench_gapbuffer_numa bench/bench_gapbuffer_numa.cpp)
target_link_libraries(bench_gapbuffer_numa PRIVATE hft_iface)
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
add_executable(test_gap_injection tests/test_gap_injection.cpp)
target_link_libraries(test_gap_injection PRIVATE hft_iface)
add_test(NAME gap_injection COMMAND test_gap_injection)

add_executable(test_market_maker tests/test_market_maker.cpp)
target_link_libraries(test_market_maker PRIVATE hft_iface)
add_test(NAME market_maker COMMAND test_market_maker)

add_executable(test_live_multicast tests/test_live_multicast.cpp)
target_link_libraries(test_live_multicast PRIVATE hft_iface)
add_test(NAME live_multicast COMMAND test_live_multicast)

# Not in ctest — used by CI to prove CHECK is active under Release (-DNDEBUG).
add_executable(test_check_macro_fails tests/test_check_macro_fails.cpp)
target_link_libraries(test_check_macro_fails PRIVATE hft_iface)
