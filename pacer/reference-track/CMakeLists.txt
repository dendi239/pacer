add_pacer_library(reference-track SOURCES reference-track.cpp HEADERS reference-track.hpp)

# hello_imgui needs nlohmann_json too and has already resolved it by the
# time we get here -- either to an imported find_package target or, where
# there is no system copy (the emscripten toolchain), to the one it vendors.
# Reuse whichever it settled on rather than resolving it a second time.
if(TARGET nlohmann_json::nlohmann_json)
    set(nlohmann_json_target nlohmann_json::nlohmann_json)
elseif(TARGET nlohmann_json)
    set(nlohmann_json_target nlohmann_json)
else()
    find_package(nlohmann_json REQUIRED)
    set(nlohmann_json_target nlohmann_json::nlohmann_json)
endif()

target_link_libraries(pacer_reference-track PUBLIC pacer::laps pacer::geometry ${nlohmann_json_target})
