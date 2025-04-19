add_executable(timeline timeline.cpp)
target_link_libraries(timeline PRIVATE
    implot::implot
    imgui::glfw3
    gpmf::gpmf
    pacer::geometry
    pacer::laps
    pacer::movie_handler
)

add_executable(destructor_test destructor_test.cpp)
