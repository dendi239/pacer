add_pacer_library(laps HEADERS laps.hpp SOURCES laps.cpp)
target_link_libraries(pacer_laps PUBLIC pacer::geometry)
