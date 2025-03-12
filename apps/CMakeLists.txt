add_executable(timeline timeline.cpp)
target_link_libraries(timeline PRIVATE
    implot::implot
    imgui::glfw3
    gpmf::gpmf
)
