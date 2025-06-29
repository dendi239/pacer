add_pacer_library(geometry SOURCES geometry.cpp HEADERS geometry.hpp)
target_link_libraries(pacer_geometry PUBLIC pacer::datatypes implot::implot)

# nanobind_add_module(
#     _pacer_geometry_impl

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

#     geometry-bindings.cpp
# )

# target_link_libraries(_pacer_geometry_impl PRIVATE pacer::geometry)
