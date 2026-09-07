add_pacer_library(session SOURCES session.cpp HEADERS session.hpp)

# nlohmann_json is already resolved by the time we get here (see
# reference-track for why the target can go by either name).
if(TARGET nlohmann_json::nlohmann_json)
    set(nlohmann_json_target nlohmann_json::nlohmann_json)
elseif(TARGET nlohmann_json)
    set(nlohmann_json_target nlohmann_json)
else()
    find_package(nlohmann_json REQUIRED)
    set(nlohmann_json_target nlohmann_json::nlohmann_json)
endif()

target_link_libraries(pacer_session PUBLIC
    pacer::laps pacer::gps-source pacer::reference-track)
target_link_libraries(pacer_session PRIVATE ${nlohmann_json_target})
