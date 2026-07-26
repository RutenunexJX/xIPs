add_executable(tst_core tst_core.cpp)
target_compile_definitions(tst_core
    PRIVATE
        XIPS_CLI_PATH="$<TARGET_FILE:xips-cli>"
        XIPS_EXAMPLE_LIBRARY="${CMAKE_SOURCE_DIR}/examples/library"
)
target_link_libraries(tst_core
    PRIVATE
        xips_core
        Qt6::Test
)
add_dependencies(tst_core xips-cli)
add_test(NAME core COMMAND tst_core)

add_executable(tst_gui_smoke tst_gui_smoke.cpp)
target_compile_definitions(tst_gui_smoke
    PRIVATE
        XIPS_EXAMPLE_LIBRARY="${CMAKE_SOURCE_DIR}/examples/library"
)
target_link_libraries(tst_gui_smoke PRIVATE xips_app Qt6::Test)
add_test(NAME gui_smoke COMMAND tst_gui_smoke)

get_target_property(_xips_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT _xips_qt_core_location)
    get_target_property(_xips_qt_core_location Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(_xips_qt_bin_dir "${_xips_qt_core_location}" DIRECTORY)
get_filename_component(_xips_qt_prefix "${_xips_qt_bin_dir}/.." ABSOLUTE)
get_filename_component(_xips_compiler_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
set_tests_properties(core gui_smoke PROPERTIES
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${_xips_qt_bin_dir};PATH=path_list_prepend:${_xips_compiler_bin_dir};QT_PLUGIN_PATH=set:${_xips_qt_prefix}/plugins"
)
set_property(TEST gui_smoke APPEND PROPERTY
    ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:offscreen"
)
