add_executable(tst_phase1
    tst_phase1.cpp
)
target_link_libraries(tst_phase1
    PRIVATE
        xips_assetindex
        Qt6::Test
)
add_test(NAME phase1 COMMAND tst_phase1)

add_executable(fake_slang fake_slang.cpp)
target_link_libraries(fake_slang PRIVATE Qt6::Core)

add_executable(tst_phase2
    tst_phase2.cpp
)
target_link_libraries(tst_phase2
    PRIVATE
        xips_assetindex
        xips_semantic
        Qt6::Test
)
add_dependencies(tst_phase2 fake_slang)
add_test(NAME phase2 COMMAND tst_phase2)

add_executable(tst_phase3
    tst_phase3.cpp
)
target_link_libraries(tst_phase3
    PRIVATE
        xips_importer
        xips_manifest
        Qt6::Test
)
add_test(NAME phase3 COMMAND tst_phase3)

add_executable(fake_test_tool fake_test_tool.cpp)
target_link_libraries(fake_test_tool PRIVATE Qt6::Core)

add_executable(tst_phase4
    tst_phase4.cpp
)
target_compile_definitions(tst_phase4
    PRIVATE
        FAKE_TEST_TOOL_PATH="$<TARGET_FILE:fake_test_tool>"
)
target_link_libraries(tst_phase4
    PRIVATE
        xips_assetindex
        xips_diff
        xips_gitservice
        xips_importer
        xips_manifest
        xips_testrunner
        Qt6::Test
)
add_dependencies(tst_phase4 fake_test_tool)
add_test(NAME phase4 COMMAND tst_phase4)

add_executable(tst_phase5
    tst_phase5.cpp
)
target_compile_definitions(tst_phase5
    PRIVATE
        XIPS_CLI_PATH="$<TARGET_FILE:xips-cli>"
        XIPS_GUI_PATH="$<TARGET_FILE:xips>"
)
target_link_libraries(tst_phase5
    PRIVATE
        xips_assetindex
        xips_integration
        xips_managed
        xips_manifest
        Qt6::Test
)
add_dependencies(tst_phase5 xips xips-cli)
add_test(NAME phase5 COMMAND tst_phase5)

add_executable(tst_gui_smoke
    tst_gui_smoke.cpp
)
target_compile_definitions(tst_gui_smoke
    PRIVATE
        XIPS_EXAMPLE_LIBRARY="${CMAKE_SOURCE_DIR}/examples/library"
)
target_link_libraries(tst_gui_smoke
    PRIVATE
        xips_app
        Qt6::Test
)
add_test(NAME gui_smoke COMMAND tst_gui_smoke)

get_target_property(_xips_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT _xips_qt_core_location)
    get_target_property(_xips_qt_core_location Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(_xips_qt_bin_dir "${_xips_qt_core_location}" DIRECTORY)
get_filename_component(_xips_qt_prefix "${_xips_qt_bin_dir}/.." ABSOLUTE)
get_filename_component(_xips_compiler_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
target_compile_definitions(tst_phase5
    PRIVATE
        XIPS_QT_BIN="${_xips_qt_bin_dir}"
        XIPS_QT_PLUGIN_PATH="${_xips_qt_prefix}/plugins"
        XIPS_COMPILER_BIN="${_xips_compiler_bin_dir}"
)
set_tests_properties(phase1 phase2 phase3 phase4 phase5 gui_smoke PROPERTIES
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${_xips_qt_bin_dir};PATH=path_list_prepend:${_xips_compiler_bin_dir};QT_PLUGIN_PATH=set:${_xips_qt_prefix}/plugins"
)
set_property(TEST gui_smoke APPEND PROPERTY
    ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:offscreen"
)
