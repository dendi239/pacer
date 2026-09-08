# File exchange with the user is host-specific: real paths on desktop,
# downloads and a page-provided file picker in the browser.
if(EMSCRIPTEN)
    set(file_transfer_source file-transfer-web.cpp)
else()
    set(file_transfer_source file-transfer.cpp)
endif()

# Native open/save dialogs, where the platform has them.
if(APPLE AND NOT EMSCRIPTEN)
    set(file_dialog_source file-dialog-mac.mm)
else()
    set(file_dialog_source file-dialog-none.cpp)
endif()

add_pacer_library(ui
    SOURCES track-picker.cpp theme.cpp ${file_transfer_source}
            ${file_dialog_source}
    HEADERS track-picker.hpp theme.hpp file-transfer.hpp file-dialog.hpp)
target_link_libraries(pacer_ui PUBLIC imgui hello_imgui)

if(APPLE)
    # theme.cpp reads the global AppleInterfaceStyle preference to find out
    # whether the system is in dark mode; file-dialog-mac.mm puts up the
    # NSOpenPanel/NSSavePanel the file pickers are built on.
    target_link_libraries(pacer_ui PRIVATE
        "-framework CoreFoundation")
    if(NOT EMSCRIPTEN)
        target_link_libraries(pacer_ui PRIVATE
            "-framework AppKit" "-framework UniformTypeIdentifiers")
    endif()
endif()
