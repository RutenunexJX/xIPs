cmake_minimum_required(VERSION 3.25)

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_id)
set(_root "${TEST_ROOT}/${_run_id}")
file(MAKE_DIRECTORY "${_root}/source" "${_root}/empty-path")
file(WRITE "${_root}/source/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.25)\nproject(ArchiveTclshProbe NONE)\n"
    "include(\"${TCLSH_MODULE}\")\n"
    "file(WRITE \"\${CMAKE_BINARY_DIR}/selected.txt\" \"\${XIPS_TEST_TCLSH}\")\n")
get_filename_component(_tclsh_directory "${TEST_TCLSH}" DIRECTORY)
find_program(_auto_tclsh NAMES tclsh tclsh86 tclsh85 PATHS "${_tclsh_directory}" NO_DEFAULT_PATH)
set(_case_count 0)

function(configure_case name search_path expected_result expected_value)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "PATH=${search_path}"
            "${CMAKE_COMMAND}" -S "${_root}/source" -B "${_root}/${name}"
            -G "${TEST_GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${TEST_MAKE_PROGRAM}"
            -DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=FALSE
            -DCMAKE_FIND_USE_CMAKE_PATH=FALSE
            -DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=FALSE
            -DCMAKE_FIND_USE_INSTALL_PREFIX=FALSE
            -DCMAKE_FIND_USE_PACKAGE_ROOT_PATH=FALSE
            -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=TRUE
            ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    file(APPEND "${_root}/configure.log"
        "\nCASE ${name} ${ARGN}\nEXIT ${result}\n${output}\n${error}\n")
    if(expected_result STREQUAL "success")
        if(NOT "${result}" STREQUAL "0")
            message(FATAL_ERROR "${name}: configure failed (${result}): ${output}\n${error}")
        endif()
        file(READ "${_root}/${name}/selected.txt" selected)
        if(NOT "${selected}" STREQUAL "${expected_value}")
            message(FATAL_ERROR "${name}: selected '${selected}', expected '${expected_value}'")
        endif()
    else()
        if("${result}" STREQUAL "0" OR NOT "${output}\n${error}" MATCHES "${expected_value}")
            message(FATAL_ERROR "${name}: expected rejection matching '${expected_value}', got ${result}: ${output}\n${error}")
        endif()
    endif()
    math(EXPR next_count "${_case_count} + 1")
    set(_case_count "${next_count}" PARENT_SCOPE)
    message(STATUS "${name} ${ARGN}: ${expected_result} verified")
endfunction()

if(_auto_tclsh)
    configure_case(fresh "${_tclsh_directory}" success "${_auto_tclsh}")
    configure_case(fresh "${_root}/empty-path" success "${_auto_tclsh}")
    configure_case(empty "${_tclsh_directory}" success "${_auto_tclsh}" "-DXIPS_TEST_TCLSH:FILEPATH=")
else()
    message(STATUS "SKIP automatic-discovery success cases: the explicit interpreter directory has no standard Tcl executable name.")
endif()
configure_case(explicit "${_root}/empty-path" success "${TEST_TCLSH}" "-DXIPS_TEST_TCLSH:FILEPATH=${TEST_TCLSH}")
configure_case(explicit "${_root}/empty-path" failure "Could not find XIPS_TEST_TCLSH" "-DXIPS_TEST_TCLSH:FILEPATH=")
configure_case(missing "${_root}/empty-path" failure "Could not find XIPS_TEST_TCLSH")
if(_auto_tclsh)
    configure_case(explicit "${_tclsh_directory}" success "${_auto_tclsh}" "-DXIPS_TEST_TCLSH:FILEPATH=")
    configure_case(missing "${_tclsh_directory}" success "${_auto_tclsh}")
endif()
configure_case(invalid "${_tclsh_directory}" failure "XIPS_TEST_TCLSH must point"
    "-DXIPS_TEST_TCLSH:FILEPATH=${_root}/absent/tclsh")
configure_case(directory "${_tclsh_directory}" failure "XIPS_TEST_TCLSH must point"
    "-DXIPS_TEST_TCLSH:FILEPATH=${_root}")
configure_case(not_tcl "${_tclsh_directory}" failure "XIPS_TEST_TCLSH is not a working Tcl"
    "-DXIPS_TEST_TCLSH:FILEPATH=${CMAKE_COMMAND}")
configure_case(false_value "${_tclsh_directory}" failure "XIPS_TEST_TCLSH must point"
    "-DXIPS_TEST_TCLSH:FILEPATH=OFF")
message(STATUS "${_case_count} Tcl discovery/cache cases passed. Logs: ${_root}/configure.log")
