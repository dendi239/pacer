add_executable(timeline timeline.cpp)
target_link_libraries(timeline PRIVATE
    imgui::glfw3
    pacer::laps
    pacer::movie-handler
    pacer::laps-display
)

add_executable(destructor_test destructor_test.cpp)
