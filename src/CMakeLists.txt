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

add_library(xips_gitservice STATIC
    gitservice/GitService.cpp
    gitservice/GitService.h
)
target_include_directories(xips_gitservice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_gitservice PUBLIC Qt6::Core)

add_library(xips_assetindex STATIC
    assetindex/AssetIndex.cpp
    assetindex/AssetIndex.h
    assetindex/AssetScanner.cpp
    assetindex/AssetScanner.h
)
target_include_directories(xips_assetindex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_assetindex
    PUBLIC
        xips_assetcore
        xips_manifest
        Qt6::Core
        Qt6::Sql
)

add_library(xips_semantic STATIC
    semantic/SlangService.cpp
    semantic/SlangService.h
)
target_include_directories(xips_semantic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_semantic PUBLIC xips_assetcore Qt6::Core)

add_library(xips_dependency STATIC
    dependency/DependencyResolver.cpp
    dependency/DependencyResolver.h
)
target_include_directories(xips_dependency PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_dependency PUBLIC xips_assetcore Qt6::Core)

add_library(xips_diff STATIC
    diff/DiffService.cpp
    diff/DiffService.h
)
target_include_directories(xips_diff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_diff
    PUBLIC
        xips_assetcore
        xips_manifest
        Qt6::Core
)

add_library(xips_testrunner STATIC
    testrunner/TestRunner.cpp
    testrunner/TestRunner.h
)
target_include_directories(xips_testrunner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_testrunner PUBLIC xips_assetcore Qt6::Core)

add_library(xips_integration STATIC
    integration/IntegrationService.cpp
    integration/IntegrationService.h
)
target_include_directories(xips_integration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_integration
    PUBLIC
        xips_assetcore
        xips_manifest
        Qt6::Core
)

add_library(xips_managed STATIC
    managed/ManagedAssetService.cpp
    managed/ManagedAssetService.h
)
target_include_directories(xips_managed PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_managed
    PUBLIC
        xips_assetcore
        xips_manifest
        Qt6::Core
)

add_library(xips_importer STATIC
    importer/ImportService.cpp
    importer/ImportService.h
)
target_include_directories(xips_importer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_importer
    PUBLIC
        xips_assetcore
        xips_dependency
        Qt6::Core
)

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
        xips_assetindex
        xips_diff
        xips_gitservice
        xips_integration
        xips_importer
        xips_managed
        xips_semantic
        xips_testrunner
        Qt6::Concurrent
        Qt6::Widgets
)

add_executable(xips
    app/main.cpp
)
target_compile_definitions(xips PRIVATE XIPS_VERSION="${PROJECT_VERSION}")
target_link_libraries(xips PRIVATE xips_app)

set_target_properties(xips PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

add_executable(xips-cli
    cli/main.cpp
)
target_compile_definitions(xips-cli PRIVATE XIPS_VERSION="${PROJECT_VERSION}")
target_link_libraries(xips-cli
    PRIVATE
        xips_assetindex
        xips_importer
        xips_integration
        xips_managed
        Qt6::Core
)
