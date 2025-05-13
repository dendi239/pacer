add_pacer_library(movie-handler SOURCES movie-handler.cpp HEADERS movie-handler.hpp)
target_link_libraries(pacer_movie-handler PUBLIC gpmf::gpmf pacer::datatypes)
