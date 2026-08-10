cmake_minimum_required(VERSION 3.15)
project(edgedepth_terminal_native_tests LANGUAGES CXX)

enable_testing()

add_executable(research_url_test research_url_test.cpp)
target_compile_features(research_url_test PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(research_url_test PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(research_url_test PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME research_url_test COMMAND research_url_test)
