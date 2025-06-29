add_pacer_library(gps-source SOURCES gps-source.cpp gps-source-dat.cpp HEADERS gps-source.hpp)
target_link_libraries(pacer_gps-source PUBLIC gpmf::gpmf pacer::datatypes)

# nanobind_add_module(_pacer_gps_source_impl gps-source-bindings.cpp)
# target_link_libraries(_pacer_gps_source_impl PRIVATE pacer::gps-source)
