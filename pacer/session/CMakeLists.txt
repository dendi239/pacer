add_pacer_library(session SOURCES session.cpp HEADERS session.hpp)
target_link_libraries(pacer_session PUBLIC
    pacer::laps pacer::gps-source pacer::reference-track)
