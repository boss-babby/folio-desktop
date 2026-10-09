# ScanCorePurity.cmake  --  run in script mode by folio_guard_core_purity()
#
# Fails the build if a UI-toolkit type has leaked into folio-core.
#
# This is the mechanical half of architecture plan §4.1. The rule is cheap to
# state and impossible to enforce by review alone once the codebase is large:
# one `#include <QString>` in a header that everything includes and the core is
# no longer portable, no longer testable headless, and no longer swappable to a
# different toolkit. Catching it at the first offending commit costs nothing.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FOLIO_SCAN_DIR)
    message(FATAL_ERROR "ScanCorePurity: FOLIO_SCAN_DIR not set")
endif()

file(GLOB_RECURSE _files "${FOLIO_SCAN_DIR}/*.h" "${FOLIO_SCAN_DIR}/*.cpp")

set(_violations "")
foreach(_f IN LISTS _files)
    file(STRINGS "${_f}" _lines)
    set(_n 0)
    foreach(_line IN LISTS _lines)
        math(EXPR _n "${_n}+1")
        # Comments may discuss Qt freely - and must, because the whole point
        # of the boundary is to explain at each seam how the adapter on the
        # other side consumes it. Only code counts.
        if(_line MATCHES "^[ \t]*//" OR _line MATCHES "^[ \t]*\\*" OR
           _line MATCHES "^[ \t]*/\\*")
            continue()
        endif()
        if(_line MATCHES "^[ \t]*#[ \t]*include[ \t]*[<\"]Q" OR
           _line MATCHES "Q_OBJECT" OR
           _line MATCHES "Q_PROPERTY" OR
           _line MATCHES "QT_BEGIN_NAMESPACE" OR
           _line MATCHES "^[ \t]*#[ \t]*include[ \t]*[<\"]Sk" OR
           _line MATCHES "^[ \t]*#[ \t]*include[ \t]*[<\"]slint")
            list(APPEND _violations "${_f}:${_n}: ${_line}")
        endif()
    endforeach()
endforeach()

list(LENGTH _files _count)

if(_violations)
    message("")
    message("folio-core purity check FAILED")
    message("  A UI-toolkit or renderer type has entered the portable core.")
    message("  See architecture plan 4.1. Put it behind an interface in")
    message("  folio-core/include/folio/core/ and implement it in the backend.")
    message("")
    foreach(_v IN LISTS _violations)
        message("  ${_v}")
    endforeach()
    message("")
    message(FATAL_ERROR "folio-core is not toolkit-free")
endif()

message(STATUS "folio-core purity: OK (${_count} files, no toolkit types)")
