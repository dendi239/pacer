add_pacer_library(reference-track SOURCES reference-track.cpp HEADERS reference-track.hpp)

find_package(nlohmann_json REQUIRED)
target_link_libraries(pacer_reference-track PUBLIC pacer::laps pacer::geometry nlohmann_json::nlohmann_json)
