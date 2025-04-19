add_library(pacer_datatypes INTERFACE)
target_include_directories(pacer_datatypes INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
add_library(pacer::datatypes ALIAS pacer_datatypes)


add_library(pacer_movie_handler movie-handler.cpp)
target_include_directories(pacer_movie_handler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pacer_movie_handler PUBLIC gpmf::gpmf pacer::datatypes)
add_library(pacer::movie_handler ALIAS pacer_movie_handler)


add_library(pacer_geometry geometry.cpp)
target_link_libraries(pacer_geometry PUBLIC pacer::datatypes implot::implot)
add_library(pacer::geometry ALIAS pacer_geometry)


add_library(pacer_laps laps.cpp)
target_include_directories(pacer_laps PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pacer_laps PUBLIC pacer::geometry)
add_library(pacer::laps ALIAS pacer_laps)
