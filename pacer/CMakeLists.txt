macro(add_pacer_library name)
    # Parse keyword arguments: SOURCES and HEADERS
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES HEADERS)
    cmake_parse_arguments(
        PACER            # Prefix for parsed variables
        "${options}"     # No flags
        "${oneValueArgs}"# No single-value args
        "${multiValueArgs}"
        ${ARGN}          # Input arguments (after `name`)
    )

    # Validate required arguments
    if(NOT DEFINED PACER_SOURCES)
        message(FATAL_ERROR "SOURCES not provided for target ${name}")
    endif()
    if(NOT DEFINED PACER_HEADERS)
        message(FATAL_ERROR "HEADERS not provided for target ${name}")
    endif()

    # Create isolated include directory in the build tree
    set(include_dir "${CMAKE_CURRENT_BINARY_DIR}/include/pacer/${name}")
    set(include_prefix pacer/${name})
    file(MAKE_DIRECTORY "${include_dir}/${include_prefix}")

    # Copy headers to build include directory
    set(copied_headers)
    foreach(header IN LISTS PACER_HEADERS)
        get_filename_component(header_abs "${header}" ABSOLUTE)
        get_filename_component(header_name "${header}" NAME)
        set(output_header "${include_dir}/${include_prefix}/${header_name}")
        add_custom_command(
            OUTPUT "${output_header}"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "${header_abs}"
                "${output_header}"
            DEPENDS "${header_abs}"
            COMMENT "Copying header ${header_name} to ${include_dir}"
        )
        list(APPEND copied_headers "${output_header}")
    endforeach()

    # Ensure headers are copied before building the library
    add_custom_target(pacer_${name}_headers DEPENDS ${copied_headers})

    # Create the library target
    add_library(pacer_${name} STATIC ${PACER_SOURCES} ${PACER_HEADERS})

    # Link headers copying to the target
    add_dependencies(pacer_${name} pacer_${name}_headers)

    # Set include directories
    target_include_directories(pacer_${name}
        # PRIVATE
        #     ${CMAKE_CURRENT_SOURCE_DIR}  # For private includes in source tree
        PUBLIC
            $<BUILD_INTERFACE:${include_dir}>  # Public includes via build tree
    )

    # Create namespaced alias
    add_library(pacer::${name} ALIAS pacer_${name})

    # put to folder pacer (xcode)
    set_property(TARGET pacer_${name} PROPERTY FOLDER "pacer")
    set_property(TARGET pacer_${name}_headers PROPERTY FOLDER "headers helpers")

endmacro()

add_subdirectory(datatypes)
add_subdirectory(geometry)
add_subdirectory(laps)
add_subdirectory(reference-track)
add_subdirectory(ui)
if(NOT EMSCRIPTEN)
    # Only the track annotator builds for wasm, and it needs none of these;
    # gps-source in particular pulls in the GPMF video reader.
    add_subdirectory(gps-source)
    add_subdirectory(live-timing)
    add_subdirectory(laps-display)
    # Depends on gps-source (and so on the GPMF reader), hence desktop-only.
    add_subdirectory(session)
    add_subdirectory(source-view)
    add_subdirectory(comparison-view)
endif()
if(NOT SKBUILD)
    # Needs CURL and a GL context; only the desktop apps use it.
    add_subdirectory(map-tiles)
endif()