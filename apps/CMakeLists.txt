if(NOT SKBUILD)
    if(NOT EMSCRIPTEN)
        hello_imgui_add_app(timeline timeline.cpp)

        target_link_libraries(timeline PRIVATE
            pacer::laps
            pacer::gps-source
            pacer::laps-display
            pacer::map-tiles
            pacer::ui
        )
    endif()

    hello_imgui_add_app(track_annotator track_annotator.cpp)
    target_link_libraries(track_annotator PRIVATE
        pacer::reference-track
        pacer::map-tiles
        pacer::ui
    )

    if(EMSCRIPTEN)
        # hello_imgui already preloads track_annotator_assets/ at the wasm
        # filesystem root. Add the repo's annotated tracks alongside it, so
        # the picker has a library to open without the user first finding a
        # file on their own disk.
        target_link_options(track_annotator PRIVATE
            "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/tracks@/tracks")

        # A screenful of decoded 256x256 tiles does not fit the default
        # 16MB heap. hello_imgui also passes -sALLOW_MEMORY_GROWTH, so this
        # is a starting size rather than a cap -- but growing the heap
        # copies it, and doing that repeatedly during the first pan is
        # visible as a stutter.
        hello_imgui_set_emscripten_target_initial_memory_megabytes(
            track_annotator 128)
    endif()

    if(NOT EMSCRIPTEN)
        add_executable(destructor_test destructor_test.cpp)

        add_executable(datparser datparser.c)
    endif()
endif()
