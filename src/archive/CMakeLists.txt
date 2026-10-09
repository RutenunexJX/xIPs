find_package(Qt6 6.10.2 EXACT REQUIRED COMPONENTS Xml CorePrivate)
add_library(xips_archive_core STATIC
    ArchiveEngine.cpp ArchiveEngine.h ArchiveScan.cpp ArchiveScan.h
    CoreContainer.cpp CoreContainer.h VivadoWorkspace.h ArchiveRuntime.cpp ArchiveRuntime.h)
target_include_directories(xips_archive_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_archive_core PUBLIC Qt6::Core Qt6::Xml PRIVATE Qt6::CorePrivate ${CMAKE_DL_LIBS})
add_library(xips_archive_ui STATIC ArchivePage.cpp ArchivePage.h ArchiveBatchPanel.cpp ArchiveBatchPanel.h
    ArchiveState.cpp ArchiveState.h ArchiveWindow.cpp ArchiveWindow.h xips-archive-resources.qrc)
target_include_directories(xips_archive_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xips_archive_ui PUBLIC xips_archive_core Qt6::Widgets)
xips_enable_strict_warnings(xips_archive_core)
xips_enable_strict_warnings(xips_archive_ui)
if(WIN32)
    set(_sevenzip "${PROJECT_SOURCE_DIR}/thirdparty/7zip")
    file(SHA256 "${_sevenzip}/7za.exe" _sevenzip_hash)
    if(NOT _sevenzip_hash STREQUAL "edbee35370e14030e4c785cf88200f42dc651c1eb4217c1e3963c38a12f099b0")
        message(FATAL_ERROR "The pinned 7-Zip runtime hash does not match.")
    endif()
    configure_file("${_sevenzip}/7za.exe" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tools/7zip/7za.exe" COPYONLY)
    install(FILES "${_sevenzip}/7za.exe" DESTINATION "${CMAKE_INSTALL_BINDIR}/tools/7zip")
    install(FILES "${_sevenzip}/7z2603-src.tar.xz" "${_sevenzip}/UPSTREAM.md"
        "${_sevenzip}/License.txt" "${_sevenzip}/COPYING.txt" "${_sevenzip}/readme.txt"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/licenses/7zip")
endif()
