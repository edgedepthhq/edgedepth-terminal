cmake_minimum_required(VERSION 3.15)
project(c_based_trader_client VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ===================================================================
# REQUIRE EMSCRIPTEN
# ===================================================================

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
            "This project requires Emscripten for WebAssembly builds.\n"
            "Usage: emcmake cmake .. && emmake make\n"
            "Install Emscripten: https://emscripten.org/docs/getting_started/downloads.html"
    )
endif()

# ===================================================================
# BUILD TYPE CONFIGURATION
# ===================================================================

# Default to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug or Release)" FORCE)
endif()

message(STATUS "===========================================")
message(STATUS "Building for WebAssembly (Emscripten)")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "===========================================")

# Suppress FetchContent deprecation warning
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# ===================================================================
# FETCH EXTERNAL DEPENDENCIES
# ===================================================================

# ────────────────────────────────────────────────────────────────────
# nlohmann/json (single header, for subscribe/unsubscribe messages)
# ────────────────────────────────────────────────────────────────────
message(STATUS "Fetching nlohmann/json v3.12.0...")
FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp
        DOWNLOAD_NO_EXTRACT TRUE
)

FetchContent_GetProperties(json)
if(NOT json_POPULATED)
    FetchContent_Populate(json)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/nlohmann)
    file(COPY ${json_SOURCE_DIR}/json.hpp
            DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include/nlohmann/)
    message(STATUS "nlohmann/json: ${CMAKE_CURRENT_BINARY_DIR}/include/nlohmann/json.hpp")
endif()

# ────────────────────────────────────────────────────────────────────
# ImGui (docking branch - pinned to pre-glBindSampler commit for WebGL2/GLES3.0 compat)
# [2026-04-24] Latest docking branch added glBindSampler which requires GLES 3.2,
# but Emscripten targets WebGL2 (GLES 3.0). Pin to last known-good commit.
# To update: check https://github.com/ocornut/imgui/commits/docking for a commit
# that doesn't use glBindSampler in imgui_impl_opengl3.cpp, update the hash below.
# ────────────────────────────────────────────────────────────────────
message(STATUS "Fetching ImGui (docking branch, pinned)...")
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG dee5bf3ec
        GIT_SHALLOW FALSE
)

FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    message(STATUS "ImGui: ${imgui_SOURCE_DIR}")
endif()
set(IMGUI_DIR ${imgui_SOURCE_DIR})

# ────────────────────────────────────────────────────────────────────
# ImPlot (pinned)
# [2026-08-06] Pinned to the commit the drawing-tools input arbitration was
# verified against (GetInputMap()/ImPlotInputMap semantics: mutable ref read
# live at EndPlot; Pan/Fit/Select index size-5 IO arrays - see
# src/ui/drawing/drawing_layer.cpp). Was GIT_TAG master; re-verify the input
# map before moving the pin.
# ────────────────────────────────────────────────────────────────────
message(STATUS "Fetching ImPlot (pinned)...")
FetchContent_Declare(
        implot
        GIT_REPOSITORY https://github.com/epezent/implot.git
        GIT_TAG d65a2bef53d32502407de3a4be80f191e2f412d7
        GIT_SHALLOW FALSE
)

FetchContent_GetProperties(implot)
if(NOT implot_POPULATED)
    FetchContent_Populate(implot)
    message(STATUS "ImPlot: ${implot_SOURCE_DIR}")
endif()
set(IMPLOT_DIR ${implot_SOURCE_DIR})

# ────────────────────────────────────────────────────────────────────
# ZSTD (v1.5.6) - Compression library
# ────────────────────────────────────────────────────────────────────
message(STATUS "Fetching ZSTD v1.5.6...")
FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        GIT_SHALLOW TRUE
)

FetchContent_GetProperties(zstd)
if(NOT zstd_POPULATED)
    FetchContent_Populate(zstd)
    message(STATUS "ZSTD: ${zstd_SOURCE_DIR}")
endif()
set(ZSTD_DIR ${zstd_SOURCE_DIR})

# ────────────────────────────────────────────────────────────────────
# Protobuf (v21.12 - no abseil dependency, WASM-compatible)
# ────────────────────────────────────────────────────────────────────
message(STATUS "Fetching Protobuf v21.12 for Emscripten...")

FetchContent_Declare(
        protobuf
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG v21.12
        GIT_SHALLOW TRUE
)

FetchContent_GetProperties(protobuf)
if(NOT protobuf_POPULATED)
    FetchContent_Populate(protobuf)
    message(STATUS "Protobuf: ${protobuf_SOURCE_DIR}")
endif()

set(PROTOBUF_SOURCE_DIR ${protobuf_SOURCE_DIR})

# Protobuf-lite sources (minimal subset for WASM)
set(PROTOBUF_LITE_SRCS
        # Core
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/arena.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/arenastring.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/extension_set.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/generated_enum_util.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/generated_message_tctable_lite.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/generated_message_util.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/implicit_weak_message.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/message_lite.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/parse_context.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/repeated_field.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/repeated_ptr_field.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/wire_format_lite.cc

        # IO
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/io/coded_stream.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/io/io_win32.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/io/strtod.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/io/zero_copy_stream.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/io/zero_copy_stream_impl.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/io/zero_copy_stream_impl_lite.cc

        # Stubs
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/stubs/bytestream.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/stubs/common.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/stubs/int128.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/stubs/status.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/stubs/stringpiece.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/stubs/stringprintf.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/stubs/structurally_valid.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/stubs/strutil.cc
        ${PROTOBUF_SOURCE_DIR}/src/google/protobuf/stubs/time.cc
)

# ===================================================================
# PROTOBUF CODE GENERATION (AUTOMATIC)
# ===================================================================

# Find host protoc compiler (must be installed on system)
find_program(PROTOC_EXECUTABLE protoc REQUIRED)
if(NOT PROTOC_EXECUTABLE)
    message(FATAL_ERROR "protoc not found! Install with: sudo apt install protobuf-compiler")
endif()
message(STATUS "Using protoc: ${PROTOC_EXECUTABLE}")

# The generated .pb.h files embed the protoc version and the v21.12 runtime
# fetched above refuses anything older. Without this guard a mismatch surfaces
# a hundred lines later as a wall of "generated by an older version of protoc"
# errors that point at generated code rather than at the real cause, which is
# the protoc on PATH. Ubuntu 22.04's protobuf-compiler is 3.12.4 and hits this
# every time.
execute_process(
        COMMAND ${PROTOC_EXECUTABLE} --version
        OUTPUT_VARIABLE PROTOC_VERSION_RAW
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${PROTOC_VERSION_RAW}")
set(PROTOC_VERSION_MAJOR ${CMAKE_MATCH_1})
set(PROTOC_VERSION_MINOR ${CMAKE_MATCH_2})
# protoc reports 21.x as "3.21.x", so accept either spelling of major 21.
if(NOT (PROTOC_VERSION_MINOR STREQUAL "21" OR PROTOC_VERSION_MAJOR STREQUAL "21"))
    message(FATAL_ERROR
            "protoc ${PROTOC_VERSION_RAW} is incompatible with the protobuf "
            "v21.12 runtime this project fetches.\n"
            "Install a 21.x protoc, for example:\n"
            "  curl -fsSLO https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protoc-21.12-linux-x86_64.zip\n"
            "  sudo unzip -o protoc-21.12-linux-x86_64.zip -d /usr/local\n"
            "Distro packages are often too old: Ubuntu 22.04 ships 3.12.4.")
endif()

# Output directory for generated files
set(PROTO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pb)
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR})

# Generated files
set(PROTO_SRCS ${PROTO_OUTPUT_DIR}/messages.pb.cc)
set(PROTO_HDRS ${PROTO_OUTPUT_DIR}/messages.pb.h)

# Custom command to generate C++ from .proto
add_custom_command(
        OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS --cpp_out=lite:${PROTO_OUTPUT_DIR}
        --proto_path=${CMAKE_SOURCE_DIR}/protos
        ${CMAKE_SOURCE_DIR}/protos/messages.proto
        DEPENDS ${CMAKE_SOURCE_DIR}/protos/messages.proto
        COMMENT "Generating C++ from messages.proto (protobuf-lite)"
        VERBATIM
)

# Create target for generated files
add_custom_target(generate_proto
        DEPENDS ${PROTO_SRCS} ${PROTO_HDRS}
        COMMENT "Ensuring protobuf files are generated"
)

message(STATUS "Protobuf generation configured:")
message(STATUS "  Input:  ${CMAKE_SOURCE_DIR}/protos/messages.proto")
message(STATUS "  Output: ${PROTO_OUTPUT_DIR}/messages.pb.{h,cc}")

# ===================================================================
# INCLUDE DIRECTORIES
# ===================================================================

include_directories(
        # Project source
        ${CMAKE_SOURCE_DIR}/src

        # Generated protobuf headers (BUILD directory)
        ${PROTO_OUTPUT_DIR}

        # Generated headers
        ${CMAKE_CURRENT_BINARY_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}

        # External libraries
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMPLOT_DIR}
        ${ZSTD_DIR}/lib
        ${PROTOBUF_SOURCE_DIR}/src
)

# ===================================================================
# SOURCE FILES
# ===================================================================

# ImGui sources
set(IMGUI_SRC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ImPlot sources
set(IMPLOT_SRC
        ${IMPLOT_DIR}/implot.cpp
        ${IMPLOT_DIR}/implot_items.cpp
        ${IMPLOT_DIR}/implot_demo.cpp
)

# ZSTD sources (decompression only)
set(ZSTD_SRC
        ${ZSTD_DIR}/lib/decompress/zstd_decompress.c
        ${ZSTD_DIR}/lib/decompress/huf_decompress.c
        ${ZSTD_DIR}/lib/decompress/zstd_ddict.c
        ${ZSTD_DIR}/lib/decompress/zstd_decompress_block.c
        ${ZSTD_DIR}/lib/common/entropy_common.c
        ${ZSTD_DIR}/lib/common/fse_decompress.c
        ${ZSTD_DIR}/lib/common/zstd_common.c
        ${ZSTD_DIR}/lib/common/xxhash.c
        ${ZSTD_DIR}/lib/common/error_private.c
)

# Project sources
set(PROJECT_SRC
        # Main
        src/main.cpp

        # Types
        src/types/types.h
        src/types/types.cpp
        src/types/flat_map.h

        # Core
        src/core/message_handler.h
        src/core/message_handler.cpp
        src/core/message_parser.h
        src/core/message_parser.cpp
        src/core/performance_tracker.h
        src/core/websocket.h
        src/core/websocket.cpp
        src/core/orderbook_manager.h
        src/core/orderbook_manager.cpp
        src/core/app_context.h
        src/core/display_time_zone.h
        src/core/display_time_zone.cpp
        src/core/logo_manager.h
        src/core/logo_manager.cpp
        src/core/message_context.h
        src/core/trade_at_price.h
        src/core/drawing_manager.h
        src/core/drawing_manager.cpp

        # Data
#        src/data/parsers.h
#        src/data/parsers.cpp

        # Stream handling
        src/stream_handler.h
        src/stream_handler.cpp

        # Rendering
        src/rendering/layout.h
        src/rendering/layout.cpp
        src/rendering/theme.h
        src/rendering/theme.cpp
        src/rendering/menu.h
        src/rendering/menu.cpp
        src/rendering/app_shell.h
        src/rendering/app_shell.cpp

        # UI Widgets
        src/ui/widget.h
        src/ui/upsell_modal.h
        src/ui/upsell_modal.cpp
        src/ui/research_moment_panel.h
        src/ui/research_moment_panel.cpp
        src/ui/chart_widget.h
        src/ui/chart_widget.cpp
        src/ui/chart_widget_scrub_preview.cpp
        src/ui/price_profile_renderer.h
        src/ui/price_profile_renderer.cpp
        src/ui/trades_widget.h
        src/ui/trades_widget.cpp
        src/ui/watchlist_widget.h
        src/ui/watchlist_widget.cpp
        src/ui/orderbook_widget.h
        src/ui/orderbook_widget.cpp
        src/ui/dom_widget.h
        src/ui/dom_widget.cpp
        src/ui/stats_widget.h
        src/ui/stats_widget.cpp

        # Generated protobuf (from build directory)
        ${PROTO_SRCS}

        # Protobuf-lite runtime
        ${PROTOBUF_LITE_SRCS}
        src/ui/custom_implot.h
        src/ui/custom_implot.cpp
        # Drawing tools (TradingView-style chart annotations)
        src/ui/drawing/drawing_types.h
        src/ui/drawing/drawing_icons.h
        src/ui/drawing/drawing_icons.cpp
        src/ui/drawing/drawing_toolbar.h
        src/ui/drawing/drawing_toolbar.cpp
        src/ui/drawing/drawing_layer.h
        src/ui/drawing/drawing_layer.cpp
        src/ui/drawing/drawing_style_editor.h
        src/ui/drawing/drawing_style_editor.cpp
        src/replayer/replay_manager.cpp
        src/replayer/replay_manager.h
        src/replayer/pack_replay_engine.cpp
        src/replayer/pack_replay_engine.h
        src/replayer/replay_history_buffer.h
        src/replayer/replay_history_buffer.cpp
        src/core/data_context.h
        src/core/data_context.cpp
        src/ui/indicators/indicator_base.h
        src/ui/indicators/volume_indicator.h
        src/ui/indicators/volume_indicator.cpp
        src/ui/indicators/vpin_indicator.cpp
        src/ui/indicators/indicator_manager.cpp
        src/ui/indicators/indicator_manager.h
        src/ui/indicators/cvd_indicator.h
        src/ui/indicators/cvd_indicator.cpp
        src/ui/indicators/rsi_indicator.h
        src/ui/indicators/rsi_indicator.cpp
        src/ui/indicators/macd_indicator.h
        src/ui/indicators/macd_indicator.cpp
        src/ui/indicators/funding_rate_indicator.h
        src/ui/indicators/funding_rate_indicator.cpp
        src/ui/indicators/oi_indicator.h
        src/ui/indicators/oi_indicator.cpp
        src/core/heatmap_manager.h
        src/core/heatmap_manager.cpp
        src/core/liquidation_heatmap_manager.h
        src/core/liquidation_heatmap_manager.cpp
        src/core/volume_profile_manager.h
        src/core/volume_profile_manager.cpp
        src/core/tpo_manager.h
        src/core/tpo_manager.cpp
        src/core/footprint_manager.h
        src/core/footprint_manager.cpp
        src/types/stack_arena.h
        src/core/heatmap_colormap.h
        src/core/heatmap_colormap.cpp
        src/rendering/shader_heatmap_resources.h
        src/rendering/shader_heatmap_resources.cpp
        src/rendering/shader_heatmap_renderer.h
        src/rendering/shader_heatmap_renderer.cpp
        src/rendering/liq_field_texture_renderer.h
        src/rendering/liq_field_texture_renderer.cpp
        src/types/frame_profiler.h
        src/core/candle_manager.cpp
        src/core/candle_manager.h
        src/core/preview_candle_store.cpp
        src/core/preview_candle_store.h
        src/core/renko_builder.cpp
        src/core/renko_builder.h
        src/core/url_router.h
        src/core/education_boot.h
        src/core/education_boot.cpp
        src/core/recorder_glue.h
        src/core/recorder_glue.cpp
        src/education/lesson_types.h
        src/education/chart_projection.h
        src/education/lesson_runtime.h
        src/education/lesson_runtime.cpp
        src/education/studio_runtime.h
        src/education/studio_runtime.cpp
        src/education/event_runtime.h
        src/education/event_runtime.cpp
        src/education/recorder_runtime.h
        src/education/recorder_runtime.cpp
        src/core/symbol_metadata.h
        src/core/symbol_metadata.cpp
        src/ui/debug_widget.h
        src/ui/debug_widget.cpp
        src/ui/positions_panel.h
        src/ui/positions_panel.cpp
        src/types/paper_trading_types.h
        src/core/debug_manager.h
        src/core/debug_manager.cpp
        src/core/paper_trading_manager.h
        src/core/paper_trading_manager.cpp
        src/core/ticker_manager.h
        src/core/ticker_manager.cpp
        src/core/analytics_manager.h
        src/core/analytics_manager.cpp
        src/core/scanner_manager.h
        src/core/scanner_manager.cpp
        src/core/indicator_series.cpp
        src/core/data_thread.h
        src/core/data_thread.cpp
        src/ui/datetime_picker.h
        src/ui/datetime_picker.cpp
)

# ===================================================================
# EXECUTABLE
# ===================================================================

add_executable(c_based_trader_client
        ${PROJECT_SRC}
        ${IMGUI_SRC}
        ${IMPLOT_SRC}
        ${ZSTD_SRC}
)

# Ensure protobuf files are generated before compiling
add_dependencies(c_based_trader_client generate_proto)

target_include_directories(c_based_trader_client PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${PROTO_OUTPUT_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

# ===================================================================
# BUILD TYPE SPECIFIC FLAGS
# ===================================================================

# Common flags for both builds
set(COMMON_COMPILE_FLAGS
        -sUSE_SDL=3
#        -sUSE_WEBGL2=1
        -msimd128
        -pthread
)

set(COMMON_LINK_FLAGS
        -sUSE_SDL=3
        -sUSE_WEBGL2=1
        -sFULL_ES3=1
        -sALLOW_MEMORY_GROWTH=1
        -sNO_EXIT_RUNTIME=1
        -lwebsocket.js
#        -sFETCH
        -msimd128
        --preload-file ${CMAKE_SOURCE_DIR}/fonts@/fonts
        -sSTACK_SIZE=5MB
        # Additions!
        --shell-file ${CMAKE_SOURCE_DIR}/src/shell.html
#        -sEXPORTED_FUNCTIONS=_main,_handle_canvas_resize
#        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap
        -sDISABLE_EXCEPTION_CATCHING=1
        -sGL_ENABLE_GET_PROC_ADDRESS=1
        -pthread
        -sPTHREAD_POOL_SIZE=2
)

# Debug-specific flags
set(DEBUG_COMPILE_FLAGS
        ${COMMON_COMPILE_FLAGS}
        -g3                          # Full debug symbols
        -O0                          # No optimization
        -sASSERTIONS=2              # Maximum runtime checks
#        -sSAFE_HEAP=1               # Memory safety checks
        -sSTACK_OVERFLOW_CHECK=2    # Stack overflow detection
)

set(DEBUG_LINK_FLAGS
        ${COMMON_LINK_FLAGS}
        -sASSERTIONS=2
        #        -sSAFE_HEAP=1
        -sSTACK_OVERFLOW_CHECK=2
        --source-map-base http://localhost:8000/
        -g3
        -sENVIRONMENT=web
        # Education bridge exports - MUST match Release. The C functions are defined
        # WITH a leading underscore in source (e.g. `void _edu_cmd_continue()`), same
        # as the pre-existing `_education_on_lesson_fetch`. Emscripten strips ONE
        # leading underscore for the JS-prefix convention, so the EXPORTED_FUNCTIONS
        # name needs a DOUBLE underscore to resolve the real symbol (`__name` → symbol
        # `_name`). JS calls them as `Module.__name` (see lessonBridge.ts + the
        # education_boot.cpp EM_ASM that already uses `__education_on_lesson_fetch`).
        -sEXPORTED_FUNCTIONS=_main,__education_on_lesson_fetch,__edu_cmd_continue,__edu_cmd_back,__edu_cmd_restart,__edu_cmd_jump,__edu_cmd_set_paused,__edu_cmd_set_speed,__edu_cmd_seek,__edu_set_viewport,__set_chart_timeframe,__studio_cmd_set_source,__studio_cmd_set_paused,__studio_cmd_set_speed,__studio_cmd_seek,__studio_cmd_skip,__studio_cmd_export_start,__studio_cmd_export_stop,__studio_cmd_arm_capture,__event_cmd_set_paused,__event_cmd_set_speed,__event_cmd_seek,__event_cmd_skip,__event_cmd_restart,__replay_cmd_scrub_hover,__recorder_on_state,__edpack_on_fetch,__display_time_zone_changed,__research_moment_result
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,lengthBytesUTF8,stringToUTF8
)

# Release-specific flags
set(RELEASE_COMPILE_FLAGS
        ${COMMON_COMPILE_FLAGS}
        -O3                          # Maximum optimization
        -DNDEBUG                     # Disable debug macros
        -flto                        # Link-time optimization
        -msimd128
)

set(RELEASE_LINK_FLAGS
        ${COMMON_LINK_FLAGS}
        -O3                          # Maximum optimization
        -flto                        # Link-time optimization
        -sASSERTIONS=0              # No runtime checks
        -sSAFE_HEAP=0               # No heap checking
        -sSTACK_OVERFLOW_CHECK=0    # No stack checking
        -sDISABLE_EXCEPTION_CATCHING=1  # Smaller binary
        # Closure DISABLED: ADVANCED_OPTIMIZATIONS mangles the hand-written JS
        # glue from -lwebsocket.js (and EM_ASM blocks), silently breaking the
        # WS message-dispatch callbacks → no live data in Release. Closure only
        # minifies the ~100KB JS loader; it does nothing for WASM execution
        # speed (that's all -O3 -flto), so the perf cost of dropping it is nil.
        # --closure 1
        # "SHELL:--closure-args=--externs=${CMAKE_SOURCE_DIR}/src/closure_externs.js"
        --closure 0
        -sENVIRONMENT=web           # Web-only (no Node.js bloat)
        -sEXPORTED_FUNCTIONS=_main,__education_on_lesson_fetch,__edu_cmd_continue,__edu_cmd_back,__edu_cmd_restart,__edu_cmd_jump,__edu_cmd_set_paused,__edu_cmd_set_speed,__edu_cmd_seek,__edu_set_viewport,__set_chart_timeframe,__studio_cmd_set_source,__studio_cmd_set_paused,__studio_cmd_set_speed,__studio_cmd_seek,__studio_cmd_skip,__studio_cmd_export_start,__studio_cmd_export_stop,__studio_cmd_arm_capture,__event_cmd_set_paused,__event_cmd_set_speed,__event_cmd_seek,__event_cmd_skip,__event_cmd_restart,__replay_cmd_scrub_hover,__recorder_on_state,__edpack_on_fetch,__display_time_zone_changed,__research_moment_result
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,lengthBytesUTF8,stringToUTF8
        # ✅ ADD THESE OPTIMIZATIONS:
#        -sFILESYSTEM=0                    # No filesystem = smaller binary
        -sMALLOC=mimalloc                 # Thread-safe malloc (emmalloc has global lock contention)
        -srender-frame-rate=200
        -sSTRICT_JS=0
        -sINITIAL_MEMORY=256MB            # Extra headroom for mimalloc per-thread arenas
        -sMAXIMUM_MEMORY=4GB              # Cap at 4GB
        -sTEXTDECODER=2                   # Faster text decoding
        -sABORTING_MALLOC=0               # Don't abort on malloc failure
        -sALLOW_TABLE_GROWTH              # Allow indirect function table to grow
        -sAUTO_NATIVE_LIBRARIES=0         # Don't auto-detect libraries
#        -sNO_FILESYSTEM                   # Explicitly disable filesystem
        -sMINIFY_HTML=1                   # Minify generated HTML
        -lGL
        -lhtml5
        -sFULL_ES3=1
        -sUSE_WEBGL2=1
)

# Apply flags based on build type
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "🚀 RELEASE BUILD - Optimized for production")
    target_compile_options(c_based_trader_client PRIVATE ${RELEASE_COMPILE_FLAGS})
    target_link_options(c_based_trader_client PRIVATE ${RELEASE_LINK_FLAGS})
else()
    message(STATUS "🔧 DEBUG BUILD - Full debugging enabled")
    target_compile_options(c_based_trader_client PRIVATE ${DEBUG_COMPILE_FLAGS})
    target_link_options(c_based_trader_client PRIVATE ${DEBUG_LINK_FLAGS})
endif()

target_link_libraries(c_based_trader_client websocket.js)

# Output as index.html
set_target_properties(c_based_trader_client PROPERTIES
        SUFFIX ".html"
        OUTPUT_NAME "index"
)

# Copy coi-serviceworker.js to build dir (COOP/COEP via service worker for threading)
add_custom_command(TARGET c_based_trader_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/src/coi-serviceworker.js
        ${CMAKE_CURRENT_BINARY_DIR}/coi-serviceworker.js
        COMMENT "Copying coi-serviceworker.js to build directory"
)

# ===================================================================
# BUILD INFORMATION
# ===================================================================

message(STATUS "===========================================")
message(STATUS "Build Configuration Summary")
message(STATUS "===========================================")
message(STATUS "CMake version:     ${CMAKE_VERSION}")
message(STATUS "C++ Standard:      C++${CMAKE_CXX_STANDARD}")
message(STATUS "Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "Target:            WebAssembly (Emscripten)")
message(STATUS "")
message(STATUS "External Libraries:")
message(STATUS "  nlohmann/json:   v3.12.0")
message(STATUS "  ImGui:           docking branch")
message(STATUS "  ImPlot:          master branch")
message(STATUS "  ZSTD:            v1.5.6")
message(STATUS "  Protobuf:        v21.12 (lite)")
message(STATUS "")
message(STATUS "Protobuf:")
message(STATUS "  Compiler:        ${PROTOC_EXECUTABLE}")
message(STATUS "  Source:          protos/messages.proto")
message(STATUS "  Generated:       ${PROTO_OUTPUT_DIR}/messages.pb.{h,cc}")
message(STATUS "")
message(STATUS "Output:")
message(STATUS "  HTML:            ${CMAKE_CURRENT_BINARY_DIR}/index.html")
message(STATUS "  WASM:            ${CMAKE_CURRENT_BINARY_DIR}/index.wasm")
message(STATUS "  JS:              ${CMAKE_CURRENT_BINARY_DIR}/index.js")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "")
    message(STATUS "⚡ Production optimizations enabled:")
    message(STATUS "  - O3 optimization")
    message(STATUS "  - Link-time optimization (LTO)")
    message(STATUS "  - Closure Compiler minification")
    message(STATUS "  - No debug symbols")
    message(STATUS "  - No runtime assertions")
    message(STATUS "  - Smaller binary size")
else()
    message(STATUS "")
    message(STATUS "🔍 Debug features enabled:")
    message(STATUS "  - Full debug symbols (g3)")
    message(STATUS "  - Runtime assertions")
    message(STATUS "  - Safe heap checking")
    message(STATUS "  - Stack overflow detection")
    message(STATUS "  - Source maps")
endif()

message(STATUS "===========================================")
