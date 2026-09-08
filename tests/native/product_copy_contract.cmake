set(product_surfaces
    "${CMAKE_CURRENT_LIST_DIR}/../../src/replayer/replay_manager.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/ui/upsell_modal.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/ui/replay_library_widget.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/rendering/app_shell.cpp")

set(retired_offer_copy
    "last 30 days"
    "30-day tick replay"
    "Unlock 30-day replay"
    "550+"
    "660+")

foreach(surface IN LISTS product_surfaces)
    file(READ "${surface}" source)
    foreach(retired IN LISTS retired_offer_copy)
        string(FIND "${source}" "${retired}" found_at)
        if(NOT found_at EQUAL -1)
            message(FATAL_ERROR "${surface} still contains retired offer copy: ${retired}")
        endif()
    endforeach()
endforeach()
