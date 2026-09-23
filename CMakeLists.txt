cmake_minimum_required(VERSION 3.25)

project(xIPs VERSION 2.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Qt6 6.10.2 EXACT REQUIRED COMPONENTS
    Core
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
set_target_properties(ElaWidgetTools PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

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
