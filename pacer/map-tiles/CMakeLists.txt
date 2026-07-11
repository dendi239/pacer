add_pacer_library(map-tiles
    SOURCES tile-math.cpp tile-store.cpp canvas-tiles.cpp implot-tiles.cpp
    HEADERS tile-math.hpp tile-store.hpp canvas-tiles.hpp implot-tiles.hpp)

find_package(CURL REQUIRED)
target_link_libraries(pacer_map-tiles PUBLIC
    pacer::geometry
    imgui
    implot::implot
    glad
    stb_hello_imgui
    CURL::libcurl)
