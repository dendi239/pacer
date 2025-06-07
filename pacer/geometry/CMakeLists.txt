add_pacer_library(geometry SOURCES geometry.cpp HEADERS geometry.hpp)
target_link_libraries(pacer_geometry PUBLIC pacer::datatypes implot::implot)

nanobind_add_module(_pacer_geometry_impl geometry-bindings.cpp)
target_link_libraries(_pacer_geometry_impl PRIVATE pacer::geometry)
