cmake_minimum_required(VERSION 3.15)
project(edgedepth_terminal_native_tests LANGUAGES CXX)

enable_testing()

function(add_terminal_native_test target source)
    add_executable(${target} ${source})
    target_compile_features(${target} PRIVATE cxx_std_20)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()

    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_terminal_native_test(research_url_test research_url_test.cpp)
add_terminal_native_test(frame_profiler_test frame_profiler_test.cpp)
add_terminal_native_test(queue_metrics_test queue_metrics_test.cpp)
