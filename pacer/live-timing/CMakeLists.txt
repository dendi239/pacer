add_pacer_library(live-timing SOURCES live-timing.cpp HEADERS live-timing.hpp)
target_link_libraries(pacer_live-timing PUBLIC pacer::geometry pacer::reference-track)
