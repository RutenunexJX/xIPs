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
    library/SnapshotLibrary.cpp
    library/SnapshotLibrary.h
    library/ContentStore.cpp
    library/ContentStore.h
    library/CatalogIndex.cpp
    library/CatalogIndex.h
    library/CatalogGroups.cpp
    library/CatalogGroups.h
    manifest/ManifestService.cpp
    manifest/ManifestService.h
)
target_include_directories(xips_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_core PUBLIC Qt6::Core Qt6::Sql)
if(BUILD_TESTING)
    target_compile_definitions(xips_core PUBLIC XIPS_ENABLE_TEST_HOOKS)
endif()

add_library(xips_app STATIC
    app/Branding.cpp
    app/Branding.h
    ${CMAKE_SOURCE_DIR}/assets/xips.qrc
    app/BrowserPanel.cpp
    app/BrowserPanel.h
    app/ElaFilePicker.cpp
    app/ElaFilePicker.h
    app/MainWindow.cpp
    app/MainWindow.h
    app/CatalogModel.cpp
    app/CatalogModel.h
    app/UiSupport.cpp
    app/UiSupport.h
)
target_include_directories(xips_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_app
    PUBLIC
        xips_core
        Qt6::Concurrent
        Qt6::Widgets
        Ela::WidgetTools
)

add_executable(xips app/main.cpp)
target_compile_definitions(xips PRIVATE XIPS_VERSION="${PROJECT_VERSION}")
target_link_libraries(xips PRIVATE xips_app)
if(WIN32)
    enable_language(RC)
    set(XIPS_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/icons/xips.ico")
    configure_file(${CMAKE_SOURCE_DIR}/assets/icons/xips.rc.in ${CMAKE_CURRENT_BINARY_DIR}/xips.rc @ONLY)
    target_sources(xips PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/xips.rc)
endif()
if(XIPS_ENABLE_SUITEAPP AND SuiteApp_FOUND)
    add_library(xips_suite STATIC integration/SuiteIntegration.cpp integration/SuiteIntegration.h)
    target_include_directories(xips_suite PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(xips_suite PUBLIC xips_app SuiteApp::suiteapp)
    target_link_libraries(xips PRIVATE xips_suite)
    target_compile_definitions(xips PRIVATE XIPS_HAS_SUITEAPP=1)
endif()
set_target_properties(xips PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

add_executable(xips-cli cli/main.cpp)
target_compile_definitions(xips-cli PRIVATE XIPS_VERSION="${PROJECT_VERSION}")
target_link_libraries(xips-cli PRIVATE xips_core)

set_target_properties(xips xips-cli PROPERTIES FOLDER "qtc_runnable")

add_library(xips_browser SHARED app/BrowserApi.cpp)
target_include_directories(xips_browser PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(xips_browser PRIVATE xips_app)
set_target_properties(xips_browser PROPERTIES PREFIX "" OUTPUT_NAME "xips-browser")
install(TARGETS xips_browser RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_SOURCE_DIR}/include/xips/BrowserApi.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/xips)
install(FILES ${CMAKE_SOURCE_DIR}/assets/icons/xips-256.png ${CMAKE_SOURCE_DIR}/assets/icons/xips.ico
    DESTINATION ${CMAKE_INSTALL_BINDIR}/assets/icons)
