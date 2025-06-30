cmake_minimum_required(VERSION 3.28)

project(pacer)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

find_package(Python 3.13
  REQUIRED COMPONENTS Interpreter Development.Module
  OPTIONAL_COMPONENTS Development.SABIModule)

find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Catch2 REQUIRED)

add_subdirectory(3rdparty)
add_subdirectory(pacer)
add_subdirectory(apps)
add_subdirectory(examples)
add_subdirectory(tests)
add_subdirectory(bindings)
