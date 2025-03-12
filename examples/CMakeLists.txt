add_executable(my_imgui_demo my_imgui_demo.cpp)
target_link_libraries(my_imgui_demo PRIVATE imgui::imgui imgui::glfw3)

add_executable(my_implot_demo my_implot_demo.cpp)
target_link_libraries(my_implot_demo PRIVATE implot::implot imgui::glfw3)

add_executable(my_gpmf_demo my_gpmf_demo.cpp)
target_link_libraries(my_gpmf_demo PUBLIC gpmf::gpmf)
