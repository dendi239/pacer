add_pacer_library(laps-display SOURCES laps-display.cpp HEADERS laps-display.hpp)
target_link_libraries(pacer_laps-display PUBLIC pacer::laps pacer::geometry implot::implot)