# Locate an XLang source checkout without assuming a drive or workspace path.
#
# Optional overrides:
#   -DXLANG_ROOT=<path>
#   XLANG_ROOT=<path> environment variable
#
# Otherwise this module walks upward from the standalone repository and checks
# common sibling layouts such as <workspace>/xlang and <parent>/CantorAI/xlang.

include(FindPackageHandleStandardArgs)

set(XLANG_ROOT "" CACHE PATH "Path to the XLang source repository")

set(_xlang_candidates)
if(XLANG_ROOT)
    list(APPEND _xlang_candidates "${XLANG_ROOT}")
elseif(DEFINED ENV{XLANG_ROOT} AND NOT "$ENV{XLANG_ROOT}" STREQUAL "")
    list(APPEND _xlang_candidates "$ENV{XLANG_ROOT}")
endif()

set(_xlang_anchor "${CMAKE_CURRENT_SOURCE_DIR}")
foreach(_xlang_depth RANGE 0 8)
    list(APPEND _xlang_candidates
        "${_xlang_anchor}/xlang"
        "${_xlang_anchor}/XLang"
        "${_xlang_anchor}/CantorAI/xlang"
        "${_xlang_anchor}/CantorAI/XLang")
    get_filename_component(_xlang_parent "${_xlang_anchor}" DIRECTORY)
    if(_xlang_parent STREQUAL _xlang_anchor)
        break()
    endif()
    set(_xlang_anchor "${_xlang_parent}")
endforeach()

set(_xlang_resolved "")
foreach(_xlang_candidate IN LISTS _xlang_candidates)
    get_filename_component(_xlang_candidate_abs "${_xlang_candidate}" ABSOLUTE)
    if(EXISTS "${_xlang_candidate_abs}/Api/xlang.h"
       AND EXISTS "${_xlang_candidate_abs}/Api/value.cpp")
        set(_xlang_resolved "${_xlang_candidate_abs}")
        break()
    endif()
endforeach()

if(_xlang_resolved)
    set(XLANG_ROOT "${_xlang_resolved}" CACHE PATH "Path to the XLang source repository" FORCE)
    set(XLANG_DIR "${XLANG_ROOT}")
    set(XLANG_API_DIR "${XLANG_ROOT}/Api")
endif()

find_package_handle_standard_args(
    XLangSource
    REQUIRED_VARS XLANG_ROOT XLANG_API_DIR)

mark_as_advanced(XLANG_API_DIR)

