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

    # Third-party headers are included with angle brackets and held to warning
    # level 0 on MSVC; our own headers use quotes and stay at /W4. Without this,
    # a file-scope name in a test (start, end, value) trips C4459 inside an
    # uninstantiated nlohmann template that GCC and Clang never diagnose, so
    # the failure only ever appears on the Windows runner. -Wshadow gives
    # GCC/Clang the first-party half of MSVC's C4456-C4459 family so a local
    # that hides a member or parameter fails here before it fails there.
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8
            /external:anglebrackets /external:W0)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Werror)
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
add_terminal_native_test(lead_lag_tracker_test lead_lag_tracker_test.cpp)
add_terminal_native_test(trade_identity_test trade_identity_test.cpp)
add_terminal_native_test(entitlements_test entitlements_test.cpp)
add_terminal_native_test(dispatch_drain_test dispatch_drain_test.cpp)
add_terminal_native_test(double_buffer_test double_buffer_test.cpp)
add_terminal_native_test(tpo_manager_test tpo_manager_test.cpp)
add_terminal_native_test(indicator_series_test indicator_series_test.cpp)

# The terminal is a pricing entry point. Keep retired plan windows and universe
# counts from reappearing independently in its four displayed offer surfaces.
add_test(
    NAME product_copy_contract
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/product_copy_contract.cmake)

add_terminal_native_test(footprint_manager_test footprint_manager_test.cpp)

# Same JSON dependency as the browser terminal; this test exercises imported
# workspace validation without requiring a graphics context.
include(FetchContent)
FetchContent_Declare(workspace_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(workspace_json)
add_terminal_native_test(workspace_document_test workspace_document_test.cpp)
target_link_libraries(workspace_document_test PRIVATE nlohmann_json::nlohmann_json)

add_terminal_native_test(reference_context_test reference_context_test.cpp)

add_terminal_native_test(liq_field_tiers_test liq_field_tiers_test.cpp)

add_terminal_native_test(census_history_test census_history_test.cpp)

add_terminal_native_test(realtime_navigation_test realtime_navigation_test.cpp)
target_link_libraries(realtime_navigation_test PRIVATE nlohmann_json::nlohmann_json)

add_terminal_native_test(observed_liquidations_test observed_liquidations_test.cpp)

add_terminal_native_test(candle_bubble_history_test candle_bubble_history_test.cpp)
target_link_libraries(candle_bubble_history_test PRIVATE nlohmann_json::nlohmann_json)

add_terminal_native_test(candle_bubble_display_test candle_bubble_display_test.cpp)

add_terminal_native_test(flow_positioning_test flow_positioning_test.cpp)
target_link_libraries(flow_positioning_test PRIVATE nlohmann_json::nlohmann_json)

# Merge rules for two windows sharing one symbol's drawings; compiles the
# production manager natively (its storage bridge is Emscripten-only).
add_terminal_native_test(drawing_merge_test drawing_merge_test.cpp)
target_sources(drawing_merge_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/core/drawing_manager.cpp)
target_link_libraries(drawing_merge_test PRIVATE nlohmann_json::nlohmann_json)

add_terminal_native_test(dispatch_queue_bound_test dispatch_queue_bound_test.cpp)

add_terminal_native_test(realtime_bubbles_test realtime_bubbles_test.cpp)
target_link_libraries(realtime_bubbles_test PRIVATE nlohmann_json::nlohmann_json)

add_terminal_native_test(liquidity_response_test liquidity_response_test.cpp)
target_link_libraries(liquidity_response_test PRIVATE nlohmann_json::nlohmann_json)
