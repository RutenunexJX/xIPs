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

add_executable(tst_snapshots tst_snapshots.cpp)
target_link_libraries(tst_snapshots PRIVATE xips_core Qt6::Test)
add_test(NAME snapshots COMMAND tst_snapshots)

add_executable(tst_workflows tst_workflows.cpp)
target_link_libraries(tst_workflows PRIVATE xips_core Qt6::Test)
target_compile_definitions(tst_workflows PRIVATE XIPS_CLI_PATH="$<TARGET_FILE:xips-cli>")
add_dependencies(tst_workflows xips-cli)
xips_enable_strict_warnings(tst_workflows)
add_test(NAME workflows COMMAND tst_workflows)

add_executable(tst_native tst_native.cpp)
target_include_directories(tst_native PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(tst_native PRIVATE XIPS_BROWSER_PATH="$<TARGET_FILE:xips_browser>")
target_link_libraries(tst_native PRIVATE xips_core Qt6::Test Qt6::Widgets Ela::WidgetTools)
add_dependencies(tst_native xips_browser)
add_test(NAME native COMMAND tst_native)

# Host-like probe deliberately does not link xips_app or either Ela runtime.
add_executable(tst_embedded tst_embedded.cpp)
target_include_directories(tst_embedded PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(tst_embedded PRIVATE XIPS_BROWSER_PATH="$<TARGET_FILE:xips_browser>")
target_link_libraries(tst_embedded PRIVATE xips_core Qt6::Test Qt6::Widgets Qt6::Concurrent)
set_target_properties(tst_embedded PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/host-probe")
xips_enable_strict_warnings(tst_embedded)
add_dependencies(tst_embedded xips_browser)
add_test(NAME embedded COMMAND tst_embedded -o -,txt)

add_executable(tst_working_model tst_working_model.cpp)
target_link_libraries(tst_working_model PRIVATE xips_app Qt6::Test)
xips_enable_strict_warnings(tst_working_model)
add_test(NAME working_model COMMAND tst_working_model -o -,txt)

add_executable(tst_catalog_model tst_catalog_model.cpp)
target_link_libraries(tst_catalog_model PRIVATE xips_app Qt6::Test)
xips_enable_strict_warnings(tst_catalog_model)
add_test(NAME catalog_model COMMAND tst_catalog_model -o -,txt)

add_executable(tst_cli tst_cli.cpp)
target_compile_definitions(tst_cli
    PRIVATE
        XIPS_CLI_PATH="$<TARGET_FILE:xips-cli>"
)
target_link_libraries(tst_cli PRIVATE xips_core Qt6::Test)
add_dependencies(tst_cli xips-cli)
add_test(NAME cli COMMAND tst_cli)

add_executable(tst_gui_smoke tst_gui_smoke.cpp)
target_compile_definitions(tst_gui_smoke
    PRIVATE
        XIPS_EXAMPLE_LIBRARY="${CMAKE_SOURCE_DIR}/examples/library"
)
target_link_libraries(tst_gui_smoke PRIVATE xips_app Qt6::Test)
add_test(NAME gui_smoke COMMAND tst_gui_smoke -o -,txt
    -o ${CMAKE_CURRENT_BINARY_DIR}/gui-smoke.xml,junitxml)

add_executable(tst_user_journey tst_user_journey.cpp)
set_target_properties(tst_user_journey PROPERTIES FOLDER "tests")
target_link_libraries(tst_user_journey PRIVATE xips_app Qt6::Test)
add_test(NAME user_journey COMMAND tst_user_journey)

add_executable(tst_browser_performance tst_browser_performance.cpp)
target_link_libraries(tst_browser_performance PRIVATE xips_app Qt6::Test)
xips_enable_strict_warnings(tst_browser_performance)

add_executable(tst_interactions tst_interactions.cpp)
target_compile_definitions(tst_interactions PRIVATE XIPS_EXAMPLE_LIBRARY="${CMAKE_SOURCE_DIR}/examples/library")
target_link_libraries(tst_interactions PRIVATE xips_app Qt6::Test)
xips_enable_strict_warnings(tst_interactions)
add_test(NAME interactions COMMAND tst_interactions -o -,txt)

add_executable(xips_fake_vivado fake_vivado.cpp)
target_link_libraries(xips_fake_vivado PRIVATE Qt6::Core)
add_executable(xips_fake_7zip fake_7zip.cpp)
target_link_libraries(xips_fake_7zip PRIVATE Qt6::Core)
add_executable(tst_archive tst_archive.cpp)
target_link_libraries(tst_archive PRIVATE xips_archive_ui Qt6::Test Qt6::CorePrivate)
get_filename_component(_archive_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
target_compile_definitions(tst_archive PRIVATE FTB_TEST_COMPILER_BIN="${_archive_compiler_bin}" FTB_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(tst_archive xips_fake_vivado xips_fake_7zip)
add_test(NAME archive COMMAND tst_archive -o -,txt)
set(XIPS_TEST_TCLSH "" CACHE FILEPATH "Tcl 8.5+ interpreter for archive script tests")
if(NOT XIPS_TEST_TCLSH)
    find_program(XIPS_TEST_TCLSH NAMES tclsh tclsh86 tclsh85 REQUIRED)
endif()
set_tests_properties(archive PROPERTIES TIMEOUT 120 ENVIRONMENT "FTB_TEST_TCLSH=${XIPS_TEST_TCLSH}")

foreach(_archive_host IN ITEMS tst_archive_host tst_archive_standalone)
    add_executable(${_archive_host} tst_archive_host.cpp)
    target_include_directories(${_archive_host} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(${_archive_host} PRIVATE XIPS_BROWSER_PATH="$<TARGET_FILE:xips_browser>"
        XIPS_TEST_HELPER_BIN="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" FTB_TEST_COMPILER_BIN="${_archive_compiler_bin}")
    target_link_libraries(${_archive_host} PRIVATE Qt6::Test Qt6::Widgets)
    add_dependencies(${_archive_host} xips_browser xips_fake_vivado)
endforeach()
target_compile_definitions(tst_archive_standalone PRIVATE XIPS_ARCHIVE_STANDALONE)
target_link_libraries(tst_archive_standalone PRIVATE xips_app)
set_target_properties(tst_archive_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/archive-host-probe")
add_test(NAME archive_host COMMAND tst_archive_host -o -,txt)
add_test(NAME archive_standalone COMMAND tst_archive_standalone -o -,txt)

get_target_property(_xips_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT _xips_qt_core_location)
    get_target_property(_xips_qt_core_location Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(_xips_qt_bin_dir "${_xips_qt_core_location}" DIRECTORY)
get_filename_component(_xips_qt_prefix "${_xips_qt_bin_dir}/.." ABSOLUTE)
get_filename_component(_xips_compiler_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(_xips_tests archive_host archive_standalone archive core snapshots workflows cli gui_smoke user_journey native interactions working_model catalog_model embedded)
set(_xips_gui_tests archive_host archive_standalone archive gui_smoke user_journey native interactions working_model catalog_model embedded)
if(TARGET xips_suite)
    add_executable(tst_suite tst_suite.cpp)
    target_compile_definitions(tst_suite PRIVATE
        XIPS_RUNTIME_PATH="$<TARGET_FILE:SuiteApp::suite-runtime>"
        XIPS_GUI_PATH="$<TARGET_FILE:xips>"
        XIPS_TEST_VERSION="${PROJECT_VERSION}")
    add_dependencies(tst_suite xips xips_browser)
    target_link_libraries(tst_suite PRIVATE xips_suite Qt6::Test)
    add_test(NAME suite COMMAND tst_suite -o -,txt
        -o ${CMAKE_CURRENT_BINARY_DIR}/suite.xml,junitxml)
    list(APPEND _xips_tests suite)
    list(APPEND _xips_gui_tests suite)
endif()
set_tests_properties(${_xips_tests} PROPERTIES
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${_xips_qt_bin_dir};PATH=path_list_prepend:${_xips_compiler_bin_dir};QT_PLUGIN_PATH=set:${_xips_qt_prefix}/plugins"
)
set_property(TEST ${_xips_gui_tests} APPEND PROPERTY
    ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:offscreen"
)
