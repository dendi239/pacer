add_pacer_library(geometry SOURCES geometry.cpp HEADERS geometry.hpp)
target_link_libraries(pacer_geometry PUBLIC pacer::datatypes implot::implot)
