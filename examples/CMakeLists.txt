add_executable(my_imgui_demo my_imgui_demo.cpp)
target_link_libraries(my_imgui_demo PRIVATE imgui
#  imgui::glfw3
)
set_property(TARGET my_imgui_demo PROPERTY FOLDER examples)

add_executable(my_implot_demo my_implot_demo.cpp)
target_link_libraries(my_implot_demo PRIVATE implot::implot imgui)
set_property(TARGET my_implot_demo PROPERTY FOLDER examples)

add_executable(my_gpmf_demo my_gpmf_demo.cpp)
target_link_libraries(my_gpmf_demo PUBLIC gpmf::gpmf)
set_property(TARGET my_gpmf_demo PROPERTY FOLDER examples)

hello_imgui_add_app(my_hello_imgui_demo hello_imgui.cpp)
