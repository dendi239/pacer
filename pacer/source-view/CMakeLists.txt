add_pacer_library(source-view SOURCES source-view.cpp HEADERS source-view.hpp)
target_link_libraries(pacer_source-view PUBLIC
    pacer::session pacer::laps-display pacer::ui implot::implot)
