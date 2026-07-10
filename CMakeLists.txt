cmake_minimum_required(VERSION 3.28)

project(pacer)

# Ensure SKBUILD is defined (scikit-build sets it to true during pip install)
if(NOT DEFINED SKBUILD)
    set(SKBUILD OFF)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

enable_testing()

find_package(Python 3.11
    REQUIRED COMPONENTS Interpreter Development.Module
    OPTIONAL_COMPONENTS Development.SABIModule)
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Catch2 REQUIRED)

add_subdirectory(3rdparty)
add_subdirectory(pacer)
if(NOT SKBUILD)
    add_subdirectory(apps)
endif()
# The following are optional components that may cause build issues during pip install.
if(NOT SKBUILD)
    add_subdirectory(examples)
    add_subdirectory(tests)
endif()
if(SKBUILD)
    add_subdirectory(bindings)
endif()
