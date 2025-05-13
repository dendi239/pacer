add_pacer_library(gps-source SOURCES gps-source.cpp HEADERS gps-source.hpp)
target_link_libraries(pacer_gps-source PUBLIC gpmf::gpmf pacer::datatypes)
