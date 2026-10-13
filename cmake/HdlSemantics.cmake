include(FetchContent)
function(xips_configure_hdl_semantics)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_DISABLE_FIND_PACKAGE_Boost TRUE)
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_PYLIB OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_INSTALL OFF CACHE BOOL "" FORCE)
set(SLANG_USE_MIMALLOC OFF CACHE BOOL "" FORCE)
set(SLANG_USE_CPPTRACE OFF CACHE BOOL "" FORCE)
set(SLANG_USE_THREADS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 407c905e45ad75fc29bf0f9bb7c5c2fd3475976f
    GIT_SHALLOW FALSE)
FetchContent_Declare(slang
    GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
    GIT_TAG ace09c5d7c9f4e28eed654d2f353c6dc792ebf67
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(slang)
if(MINGW)
    target_compile_options(slang_slang PRIVATE -Wa,-mbig-obj)
endif()
# Collect the actual static dependencies' notices for both install and packaging.
FetchContent_GetProperties(fmt SOURCE_DIR _xips_fmt_source)
set(_notices "${CMAKE_BINARY_DIR}/hdl-licenses")
file(MAKE_DIRECTORY "${_notices}/slang" "${_notices}/fmt")
file(COPY "${slang_SOURCE_DIR}/LICENSES/MIT.txt" "${slang_SOURCE_DIR}/LICENSES/BSL-1.0.txt"
    DESTINATION "${_notices}/slang")
file(COPY "${_xips_fmt_source}/LICENSE" DESTINATION "${_notices}/fmt")
file(GLOB_RECURSE _slang_headers "${slang_SOURCE_DIR}/include/*.h" "${slang_SOURCE_DIR}/source/*.h" "${slang_SOURCE_DIR}/source/*.cpp")
set(_copyrights "Copyright (c) 2015-2026 Michael Popoloski")
foreach(_header IN LISTS _slang_headers)
    file(STRINGS "${_header}" _lines REGEX "SPDX-FileCopyrightText:")
    list(APPEND _copyrights ${_lines})
endforeach()
foreach(_external IN ITEMS boost_unordered.hpp expected.hpp)
    file(STRINGS "${slang_SOURCE_DIR}/external/${_external}" _lines REGEX "^// Copyright")
    list(APPEND _copyrights ${_lines})
endforeach()
list(REMOVE_DUPLICATES _copyrights)
list(SORT _copyrights)
string(JOIN "\n" _copyright_text ${_copyrights})
file(WRITE "${_notices}/slang/NOTICE.txt"
    "Slang v10.0 ace09c5d7c9f4e28eed654d2f353c6dc792ebf67 (MIT)\nhttps://github.com/MikePopoloski/slang\nVendored boost_unordered and expected-lite (BSL-1.0).\nSource copyright notices:\n${_copyright_text}\n")
install(DIRECTORY "${_notices}/" DESTINATION "licenses")
endfunction()
xips_configure_hdl_semantics()
