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

# ESP32-S3 firmware cross-build. Can't be an add_subdirectory (different
# toolchain), so it's an external project sharing firmware/build with idf.py.
# Needs the IDF env from scripts/idf-env.sh (activated by pixi).
option(PACER_BUILD_FIRMWARE "Build the ESP32-S3 firmware (requires ESP-IDF)" OFF)
if(PACER_BUILD_FIRMWARE)
    if(NOT DEFINED ENV{IDF_PATH})
        message(FATAL_ERROR "PACER_BUILD_FIRMWARE needs IDF_PATH set; run under pixi (scripts/idf-env.sh)")
    endif()
    include(ExternalProject)
    ExternalProject_Add(firmware
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/firmware
        BINARY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/firmware/build
        CMAKE_GENERATOR Ninja
        CMAKE_ARGS
            -DIDF_TARGET=esp32s3
            -DCMAKE_TOOLCHAIN_FILE=$ENV{IDF_PATH}/tools/cmake/toolchain-esp32s3.cmake
        BUILD_ALWAYS TRUE
        INSTALL_COMMAND ""
        USES_TERMINAL_BUILD TRUE
    )
endif()
