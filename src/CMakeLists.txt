add_library(xips_core STATIC
    assetcore/Asset.h
    assetcore/JsonUtil.cpp
    assetcore/JsonUtil.h
    integration/IntegrationService.cpp
    integration/IntegrationService.h
    library/AssetLibraryService.cpp
    library/AssetLibraryService.h
    library/AssetScanner.cpp
    library/AssetScanner.h
    library/FileSystemUtil.cpp
    library/FileSystemUtil.h
    manifest/ManifestService.cpp
    manifest/ManifestService.h
)
target_include_directories(xips_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_core PUBLIC Qt6::Core)

add_library(xips_app STATIC
    app/AssetTableModel.cpp
    app/AssetTableModel.h
    app/LibraryController.cpp
    app/LibraryController.h
    app/MainWindow.cpp
    app/MainWindow.h
)
target_include_directories(xips_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_app
    PUBLIC
        xips_core
        Qt6::Concurrent
        Qt6::Widgets
)

add_executable(xips app/main.cpp)
target_compile_definitions(xips PRIVATE XIPS_VERSION="${PROJECT_VERSION}")
target_link_libraries(xips PRIVATE xips_app)
set_target_properties(xips PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

add_executable(xips-cli cli/main.cpp)
target_compile_definitions(xips-cli PRIVATE XIPS_VERSION="${PROJECT_VERSION}")
target_link_libraries(xips-cli PRIVATE xips_core)

set_target_properties(xips xips-cli PROPERTIES FOLDER "qtc_runnable")
