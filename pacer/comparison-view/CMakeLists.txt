add_pacer_library(comparison-view
    SOURCES comparison-view.cpp HEADERS comparison-view.hpp)
target_link_libraries(pacer_comparison-view PUBLIC
    pacer::session pacer::reference-track implot::implot imgui)
