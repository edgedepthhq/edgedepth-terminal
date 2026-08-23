cmake_minimum_required(VERSION 3.15)
project(edgedepth_terminal_native_tests LANGUAGES CXX)

enable_testing()

# std::thread needs -pthread on GNU/Linux. Threads is a stock CMake module, so
# this adds no dependency the harness did not already have.
find_package(Threads REQUIRED)

function(add_terminal_native_test target source)
    add_executable(${target} ${source})
    target_compile_features(${target} PRIVATE cxx_std_20)
    # So a test can include a src/ translation unit whose own includes are
    # written relative to src/ (e.g. renko_builder.cpp's "core/renko_builder.h").
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()

    target_link_libraries(${target} PRIVATE Threads::Threads)

    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_terminal_native_test(research_url_test research_url_test.cpp)
add_terminal_native_test(frame_profiler_test frame_profiler_test.cpp)
add_terminal_native_test(queue_metrics_test queue_metrics_test.cpp)
add_terminal_native_test(reach_math_test reach_math_test.cpp)
add_terminal_native_test(renko_builder_test renko_builder_test.cpp)
add_terminal_native_test(flat_map_test flat_map_test.cpp)
add_terminal_native_test(url_router_test url_router_test.cpp)
add_terminal_native_test(entitlements_test entitlements_test.cpp)
add_terminal_native_test(dispatch_drain_test dispatch_drain_test.cpp)
add_terminal_native_test(double_buffer_test double_buffer_test.cpp)
add_terminal_native_test(tpo_manager_test tpo_manager_test.cpp)
add_terminal_native_test(indicator_series_test indicator_series_test.cpp)
