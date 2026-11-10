# An empty user/cache value means automatic discovery, not a successful lookup.
if(DEFINED XIPS_TEST_TCLSH AND "${XIPS_TEST_TCLSH}" STREQUAL "")
    unset(XIPS_TEST_TCLSH CACHE)
    unset(XIPS_TEST_TCLSH)
endif()
find_program(XIPS_TEST_TCLSH NAMES tclsh tclsh86 tclsh85
    DOC "Tcl 8.5+ interpreter for archive script tests (empty to auto-detect)"
    REQUIRED)

# find_program deliberately preserves non-empty user/cache selections.
if(NOT IS_ABSOLUTE "${XIPS_TEST_TCLSH}" OR
   NOT EXISTS "${XIPS_TEST_TCLSH}" OR IS_DIRECTORY "${XIPS_TEST_TCLSH}")
    message(FATAL_ERROR
        "XIPS_TEST_TCLSH must point to an existing Tcl 8.5+ executable: '${XIPS_TEST_TCLSH}'. "
        "Set a valid absolute path, or clear XIPS_TEST_TCLSH to auto-detect.")
endif()
execute_process(
    COMMAND "${XIPS_TEST_TCLSH}" "${CMAKE_CURRENT_LIST_DIR}/probe_tclsh.tcl"
    RESULT_VARIABLE _xips_tclsh_result
    OUTPUT_VARIABLE _xips_tclsh_output
    ERROR_VARIABLE _xips_tclsh_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    TIMEOUT 5)
if(NOT "${_xips_tclsh_result}" STREQUAL "0" OR
   NOT _xips_tclsh_output MATCHES "^XIPS_TCLSH_VERSION=([0-9]+\\.[0-9]+(\\.[0-9]+)?)$")
    message(FATAL_ERROR
        "XIPS_TEST_TCLSH is not a working Tcl 8.5+ interpreter: '${XIPS_TEST_TCLSH}'. "
        "Probe result: ${_xips_tclsh_result}. ${_xips_tclsh_error}")
endif()
if(CMAKE_MATCH_1 VERSION_LESS 8.5)
    message(FATAL_ERROR "XIPS_TEST_TCLSH requires Tcl 8.5+, found ${CMAKE_MATCH_1}: '${XIPS_TEST_TCLSH}'.")
endif()
unset(_xips_tclsh_result)
unset(_xips_tclsh_output)
unset(_xips_tclsh_error)
