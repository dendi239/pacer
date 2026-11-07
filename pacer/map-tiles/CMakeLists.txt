# The tile downloader has one backend per platform: blocking curl on worker
# threads for desktop, the browser's own fetch for wasm.
if(EMSCRIPTEN)
    set(tile_loader_source tile-loader-fetch.cpp)
else()
    set(tile_loader_source tile-loader-curl.cpp)
endif()

add_pacer_library(map-tiles
    SOURCES tile-math.cpp tile-queue.cpp tile-store.cpp canvas-tiles.cpp
            implot-tiles.cpp ${tile_loader_source}
    HEADERS tile-math.hpp tile-queue.hpp tile-store.hpp canvas-tiles.hpp
            implot-tiles.hpp tile-loader.hpp)

target_link_libraries(pacer_map-tiles PUBLIC
    pacer::geometry
    imgui
    implot::implot
    # For the GL loader and the HELLOIMGUI_USE_* defines that pick it; on
    # desktop this is glad, under emscripten it is GLES3.
    hello_imgui
    stb_hello_imgui)

# Google's session reply is JSON. nlohmann_json is already resolved by the
# time we get here (see reference-track for why the target can go by either
# name).
if(TARGET nlohmann_json::nlohmann_json)
    set(nlohmann_json_target nlohmann_json::nlohmann_json)
elseif(TARGET nlohmann_json)
    set(nlohmann_json_target nlohmann_json)
else()
    find_package(nlohmann_json REQUIRED)
    set(nlohmann_json_target nlohmann_json::nlohmann_json)
endif()
target_link_libraries(pacer_map-tiles PRIVATE ${nlohmann_json_target})

if(EMSCRIPTEN)
    # emscripten_fetch lives in a separate library that is not linked by
    # default; it must reach anything linking map-tiles, hence PUBLIC.
    target_link_options(pacer_map-tiles PUBLIC -sFETCH)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(pacer_map-tiles PUBLIC CURL::libcurl)
endif()
