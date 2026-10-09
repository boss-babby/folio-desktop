# ScanHardcodedTokens.cmake  --  run in script mode by folio_guard_design_tokens()
#
# Fails the build if a UI source hard-codes a colour or an animation duration
# that should have come from design-tokens.json.
#
# Architecture plan §11. The brief is "the same UI design everywhere" and the
# demo has 38 distinct motion timings. The only way those stay identical across
# six platforms over a year of edits is if there is exactly one place they can
# be written down, and a machine that notices when someone types #FF5C38 into a
# source file at 2am because it was faster than looking it up.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FOLIO_SCAN_DIR)
    message(FATAL_ERROR "ScanHardcodedTokens: FOLIO_SCAN_DIR not set")
endif()
if(NOT EXISTS "${FOLIO_SCAN_DIR}")
    message(STATUS "design-token check: ${FOLIO_SCAN_DIR} absent, skipping")
    return()
endif()

file(GLOB_RECURSE _files
    "${FOLIO_SCAN_DIR}/*.h" "${FOLIO_SCAN_DIR}/*.cpp" "${FOLIO_SCAN_DIR}/*.qml")

set(_violations "")
foreach(_f IN LISTS _files)
    get_filename_component(_name "${_f}" NAME)
    # The generated token header is the one file allowed to contain literals -
    # it is the output of tools/token-gen, not hand-written.
    if(_name STREQUAL "DesignTokens.h" OR _name STREQUAL "DesignTokens.cpp")
        continue()
    endif()

    file(STRINGS "${_f}" _lines)
    set(_n 0)
    foreach(_line IN LISTS _lines)
        math(EXPR _n "${_n}+1")
        if(_line MATCHES "//" OR _line MATCHES "^[ \t]*\\*")
            continue()                       # comments may say #7C3AED freely
        endif()
        if(_line MATCHES "\"#[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]")
            list(APPEND _violations "${_f}:${_n}: hard-coded colour -- ${_line}")
        elseif(_line MATCHES "duration[ \t]*[:=][ \t]*[0-9]+")
            list(APPEND _violations "${_f}:${_n}: hard-coded duration -- ${_line}")
        endif()
    endforeach()
endforeach()

if(_violations)
    message("")
    message("design-token check FAILED")
    message("  Colours and durations live in design-tokens.json and reach code")
    message("  through the generated DesignTokens header. See plan 11.")
    message("")
    foreach(_v IN LISTS _violations)
        message("  ${_v}")
    endforeach()
    message("")
    message(FATAL_ERROR "hard-coded design values in ${FOLIO_SCAN_DIR}")
endif()

list(LENGTH _files _count)
message(STATUS "design-token check: OK (${_count} files)")
