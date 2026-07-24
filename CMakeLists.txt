cmake_minimum_required(VERSION 3.25)

project(xIPs VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Concurrent
    Sql
    Widgets
)

include(GNUInstallDirs)
include(CTest)

add_subdirectory(src)

if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

install(TARGETS xips xips-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .
)
install(DIRECTORY examples/library
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xips/examples
)
install(FILES README.md PLAN.md GOAL.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
