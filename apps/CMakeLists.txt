if(NOT SKBUILD)
    find_package(CURL REQUIRED)

    hello_imgui_add_app(timeline timeline.cpp)

    target_link_libraries(timeline PRIVATE
        pacer::laps
        pacer::gps-source
        pacer::laps-display
    )

    hello_imgui_add_app(track_annotator track_annotator.cpp)
    target_link_libraries(track_annotator PRIVATE CURL::libcurl)

    if(EMSCRIPTEN)
        set_target_properties(timeline PROPERTIES SUFFIX ".js")
        set_target_properties(track_annotator PROPERTIES SUFFIX ".js")
    endif()

    add_executable(destructor_test destructor_test.cpp)

    add_executable(datparser datparser.c)
endif()
