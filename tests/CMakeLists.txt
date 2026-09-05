add_executable(test_coordinate_system test_coordinate_system.cpp)
target_link_libraries(test_coordinate_system PRIVATE
    pacer::geometry
    Catch2::Catch2WithMain)

add_test(
    NAME test_coordinate_system
    COMMAND test_coordinate_system
)

set_property(TARGET test_coordinate_system PROPERTY FOLDER "tests")

add_executable(test_live_timing test_live_timing.cpp)
target_link_libraries(test_live_timing PRIVATE
    pacer::datatypes
    pacer::geometry
    pacer::gps-source
    pacer::laps
    pacer::live-timing
    pacer::reference-track
    Catch2::Catch2WithMain)

add_test(
    NAME test_live_timing
    COMMAND test_live_timing
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

set_property(TARGET test_live_timing PROPERTY FOLDER "tests")

# Diagnostic: replays a firmware .dat log through LiveTiming and prints where
# the delta stalls. Not a test — run it by hand against a session log.
add_executable(replay_dat replay_dat.cpp)
target_link_libraries(replay_dat PRIVATE
    pacer::gps-source
    pacer::live-timing
    pacer::reference-track)

set_property(TARGET replay_dat PROPERTY FOLDER "tests")

add_executable(test_tile_queue test_tile_queue.cpp)
target_link_libraries(test_tile_queue PRIVATE
    pacer::map-tiles
    Catch2::Catch2WithMain)

add_test(
    NAME test_tile_queue
    COMMAND test_tile_queue
)

set_property(TARGET test_tile_queue PROPERTY FOLDER "tests")
