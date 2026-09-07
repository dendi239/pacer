# File exchange with the user is host-specific: real paths on desktop,
# downloads and a page-provided file picker in the browser.
if(EMSCRIPTEN)
    set(file_transfer_source file-transfer-web.cpp)
else()
    set(file_transfer_source file-transfer.cpp)
endif()

add_pacer_library(ui
    SOURCES track-picker.cpp theme.cpp ${file_transfer_source}
    HEADERS track-picker.hpp theme.hpp file-transfer.hpp)
target_link_libraries(pacer_ui PUBLIC imgui hello_imgui)

if(APPLE)
    # theme.cpp reads the global AppleInterfaceStyle preference to find out
    # whether the system is in dark mode.
    target_link_libraries(pacer_ui PRIVATE "-framework CoreFoundation")
endif()
