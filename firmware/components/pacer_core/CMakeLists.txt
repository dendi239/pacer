# Compiles the portable pacer core (datatypes, geometry, laps,
# reference-track, live-timing) straight from the repo tree as an IDF
# component. Headers resolve as <pacer/...> with the repo root on the
# include path; nlohmann/json comes from the submodule in
# 3rdparty/nlohmann (the desktop build uses find_package instead).
get_filename_component(PACER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

idf_component_register(
    SRCS
        "${PACER_ROOT}/pacer/datatypes/datatypes.cpp"
        "${PACER_ROOT}/pacer/geometry/geometry.cpp"
        "${PACER_ROOT}/pacer/laps/laps.cpp"
        "${PACER_ROOT}/pacer/reference-track/reference-track.cpp"
        "${PACER_ROOT}/pacer/live-timing/live-timing.cpp"
    INCLUDE_DIRS
        "${PACER_ROOT}"
        "${PACER_ROOT}/3rdparty/nlohmann/include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++2b)
