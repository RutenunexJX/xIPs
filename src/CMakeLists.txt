add_library(xips_assetcore STATIC
    assetcore/Asset.cpp
    assetcore/Asset.h
    assetcore/JsonUtil.cpp
    assetcore/JsonUtil.h
)
target_include_directories(xips_assetcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_assetcore PUBLIC Qt6::Core)

add_library(xips_manifest STATIC
    manifest/ManifestService.cpp
    manifest/ManifestService.h
)
target_include_directories(xips_manifest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_manifest PUBLIC xips_assetcore Qt6::Core)

add_library(xips_assetscan STATIC
    assetindex/AssetScanner.cpp
    assetindex/AssetScanner.h
)
target_include_directories(xips_assetscan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_assetscan PUBLIC xips_manifest Qt6::Core)

add_library(xips_assetlibrary STATIC
    library/AssetLibraryService.cpp
    library/AssetLibraryService.h
)
target_include_directories(xips_assetlibrary PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_assetlibrary
    PUBLIC
        xips_assetscan
        xips_manifest
        Qt6::Core
)

add_library(xips_integration STATIC
    integration/IntegrationService.cpp
    integration/IntegrationService.h
)
target_include_directories(xips_integration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_integration PUBLIC Qt6::Core)

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
        xips_assetlibrary
        xips_integration
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
target_link_libraries(xips-cli
    PRIVATE
        xips_assetlibrary
        xips_integration
        Qt6::Core
)

# Qt Creator should offer only the two application entry points as run targets.
set_target_properties(xips xips-cli PROPERTIES FOLDER "qtc_runnable")
