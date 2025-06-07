###############################################################################
#                                   imgui                                     #
###############################################################################

# To use this library, you need to link against:
#  - base imgui library: imgui::imgui
#  - specific backend library, currently only imgui::glfw3 is supported.

file(GLOB IMGUI_SRC ${CMAKE_CURRENT_SOURCE_DIR}/imgui/*.cpp)
add_library(imgui ${IMGUI_SRC})
target_include_directories(imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
add_library(imgui::imgui ALIAS imgui)
set_property(TARGET imgui PROPERTY FOLDER "3rdparty")

add_library(imgui_glfw3
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui_glfw3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends)
target_link_libraries(imgui_glfw3 imgui::imgui glfw OpenGL::GL)
set_property(TARGET imgui_glfw3 PROPERTY FOLDER "3rdparty")
add_library(imgui::glfw3 ALIAS imgui_glfw3)


###############################################################################
#                                   implot                                    #
###############################################################################

# ImPlot builds on top of imgui, so don't forget to link against respective backend.

file(GLOB IMPLOT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/implot/*.cpp)
add_library(implot ${IMPLOT_SRC})
target_include_directories(implot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/implot)
target_link_libraries(implot imgui::imgui)
set_property(TARGET implot PROPERTY FOLDER "3rdparty")
add_library(implot::implot ALIAS implot)


###############################################################################
#                                    gpmf                                     #
###############################################################################

add_library(gpmf
    ${CMAKE_CURRENT_SOURCE_DIR}/gpmf-parser/GPMF_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gpmf-parser/GPMF_utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gpmf-parser/demo/GPMF_mp4reader.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gpmf-parser/demo/GPMF_print.c
)
target_include_directories(gpmf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gpmf-parser)
set_property(TARGET gpmf PROPERTY FOLDER "3rdparty")
add_library(gpmf::gpmf ALIAS gpmf)

###############################################################################
#                                  nanobind                                   #
###############################################################################

add_subdirectory(nanobind)
