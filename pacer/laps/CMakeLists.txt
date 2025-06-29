add_pacer_library(laps HEADERS laps.hpp SOURCES laps.cpp)
target_link_libraries(pacer_laps PUBLIC pacer::geometry)

# nanobind_add_module(_pacer_laps
#     # Build libnanobind statically and merge it into the
#     # extension (which itself remains a shared library)
#     #
#     # If your project builds multiple extensions, you can
#     # replace this flag by NB_SHARED to conserve space by
#     # reusing a shared libnanobind across libraries
#     NB_STATIC

#     # Target the stable ABI for Python 3.12+, which reduces
#     # the number of binary wheels that must be built. This
#     # does nothing on older Python versions
#     STABLE_ABI

#     laps-bindings.cpp)
# target_link_libraries(_pacer_laps PRIVATE pacer::laps)

# if (SKBUILD)
#     nanobind_add_stub(
#         _pacer_laps_stubs
#         MODULE _pacer_laps
        
#         OUTPUT _pacer_laps.pyi
#         PYTHON_PATH $<TARGET_FILE_DIR:_pacer_laps>
#         DEPENDS _pacer_laps
#     )
#     install(TARGETS _pacer_laps LIBRARY DESTINATION pacer/laps)
# endif()
