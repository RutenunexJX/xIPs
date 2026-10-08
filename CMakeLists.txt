cmake_minimum_required(VERSION 3.25)

project(xIPs VERSION 2.13.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Qt6 6.10.2 EXACT REQUIRED COMPONENTS
    Core
    Sql
    Concurrent
    Widgets
    WidgetsPrivate
)

include(GNUInstallDirs)
include(CTest)
option(XIPS_ENABLE_SUITEAPP "Enable optional AppSuite integration" ON)
if(XIPS_ENABLE_SUITEAPP)
    find_package(SuiteApp 1 CONFIG QUIET)
endif()

set(QT_VERSION_MAJOR 6)
set(ELAWIDGETTOOLS_BUILD_STATIC_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/elawidgettools EXCLUDE_FROM_ALL SYSTEM)
set_target_properties(ElaWidgetTools PROPERTIES OUTPUT_NAME "XipsEla"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

file(GLOB_RECURSE _ela_sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_SOURCE_DIR}/thirdparty/elawidgettools"
    "thirdparty/elawidgettools/*.h" "thirdparty/elawidgettools/*.cpp"
    "thirdparty/elawidgettools/*.cmake" "thirdparty/elawidgettools/CMakeLists.txt")
list(SORT _ela_sources)
set(_ela_inventory "")
foreach(_ela_source IN LISTS _ela_sources)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/thirdparty/elawidgettools/${_ela_source}")
    file(READ "${CMAKE_SOURCE_DIR}/thirdparty/elawidgettools/${_ela_source}" _ela_content)
    string(REPLACE "\r\n" "\n" _ela_content "${_ela_content}")
    string(SHA256 _ela_hash "${_ela_content}")
    string(APPEND _ela_inventory "${_ela_source}:${_ela_hash}\n")
endforeach()
string(SHA256 XIPS_ELA_SOURCE_SHA256 "${_ela_inventory}")
set(XIPS_CAPABILITIES_JSON "{\"schema\":\"xips.browser-capabilities/v1\",\"version\":\"${PROJECT_VERSION}\",\"nativeSurfaceAbi\":1,\"elaRuntime\":\"XipsEla\",\"elaBaseline\":\"454cac2d-p27\",\"elaPatchLevel\":32,\"elaSourceSha256\":\"${XIPS_ELA_SOURCE_SHA256}\",\"qt\":\"${Qt6_VERSION}\",\"capabilities\":[\"private-ela-runtime\",\"working-files\",\"group-members\",\"version-deletion\",\"panel-state-v2\",\"host-theme\",\"immutable-revisions\",\"interruptible-combo\",\"precision-wheel\",\"native-text-scroll-units\",\"split-ratios\",\"english-edit-menu\",\"interruptible-menu\"]}")
file(WRITE "${CMAKE_BINARY_DIR}/xips-capabilities.json" "${XIPS_CAPABILITIES_JSON}\n")
configure_file(src/app/BuildCapabilities.h.in generated/BuildCapabilities.h @ONLY)

option(XIPS_WARNINGS_AS_ERRORS
    "Treat compiler warnings as errors for xIPs targets"
    OFF
)

function(xips_enable_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(XIPS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(XIPS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_subdirectory(src)

if(BUILD_TESTING)
    find_package(Qt6 6.10.2 EXACT REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

foreach(_xips_target IN ITEMS xips_core xips_app xips xips-cli xips_browser xips_suite tst_suite
                              tst_core tst_snapshots tst_cli tst_gui_smoke tst_user_journey tst_native)
    if(TARGET ${_xips_target})
        xips_enable_strict_warnings(${_xips_target})
    endif()
endforeach()

install(TARGETS xips xips-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .
)
install(FILES README.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
