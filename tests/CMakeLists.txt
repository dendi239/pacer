add_executable(test_coordinate_system test_coordinate_system.cpp)
target_link_libraries(test_coordinate_system PRIVATE
    pacer::geometry
    Catch2::Catch2WithMain)

add_test(
    NAME test_coordinate_system
    COMMAND test_coordinate_system
)

set_property(TARGET test_coordinate_system PROPERTY FOLDER "tests")