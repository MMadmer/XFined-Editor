# Shared build rules ported from Source/Default.props + Default.Release.props.
# Parity notes:
#  - Release compiles with BOTH NDEBUG (per-project) and DEBUG (Default.Release.props)
#  - warnings are fully off (/W0), RTTI on, C++14, /arch:AVX, /MD, LTCG
#  - WIN32 is intentionally NOT defined on x64
#  - outputs: dlls/exes -> Bin/x64/<cfg>, import+static libs -> Lib/x64/<cfg>

set(XRAY_ROOT    "${CMAKE_SOURCE_DIR}")
set(XRAY_SOURCE  "${XRAY_ROOT}/Source")
set(XRAY_CFG_DIR "x64/${CMAKE_BUILD_TYPE}")
set(XRAY_BIN     "${XRAY_ROOT}/Bin/${XRAY_CFG_DIR}")
set(XRAY_LIB     "${XRAY_ROOT}/Lib/${XRAY_CFG_DIR}")

option(XRAY_LTO "Whole-program optimization (/GL + /LTCG) like the old vcxproj" ON)

# every target routes through this to get the Default.props behaviour
function(xray_common target)
    cmake_parse_arguments(ARG "NO_MBCS" "PCH" "" ${ARGN})

    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${XRAY_BIN}"
        LIBRARY_OUTPUT_DIRECTORY "${XRAY_BIN}"
        ARCHIVE_OUTPUT_DIRECTORY "${XRAY_LIB}"
        PDB_OUTPUT_DIRECTORY     "${XRAY_BIN}"
    )

    target_compile_definitions(${target} PRIVATE
        _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS
        _WINDLL
        _SECURE_SCL=0
        _ITERATOR_DEBUG_LEVEL=0
        # the old v142 STL still shipped auto_ptr/binary_function; the modern
        # one removes them unless this compatibility switch is set
        _HAS_AUTO_PTR_ETC=1
        $<$<CONFIG:Release>:DEBUG>      # sic — Default.Release.props does exactly this
        $<$<CONFIG:Release>:NDEBUG>
    )
    if (NOT ARG_NO_MBCS)
        target_compile_definitions(${target} PRIVATE _MBCS)
    endif()

    target_include_directories(${target} PRIVATE
        "${XRAY_ROOT}/SDK/Include"
        "${XRAY_SOURCE}/XrRender/Public"
        "${XRAY_SOURCE}/External/Public"
        "${XRAY_SOURCE}/External/RedImage"
    )

    target_compile_options(${target} PRIVATE
        /W0 /GR /Oi /Gy /arch:AVX
        # restores the C++03 functional helpers the modern STL dropped
        $<$<COMPILE_LANGUAGE:CXX>:/FI${XRAY_SOURCE}/xr_msvc_stl_compat.h>
    )
    # /std:c++14 via the standard property so cmake does not fight us
    set_target_properties(${target} PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

    if (XRAY_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${target} PRIVATE /GL)
        target_link_options(${target} PRIVATE /LTCG)
    endif()

    target_link_directories(${target} PRIVATE
        "${XRAY_LIB}"
        "${XRAY_ROOT}/SDK/Lib/x64"
    )
    target_link_options(${target} PRIVATE /LARGEADDRESSAWARE /SUBSYSTEM:WINDOWS)

    if (ARG_PCH)
        target_precompile_headers(${target} PRIVATE "${ARG_PCH}")
        # no vcxproj ever fed a .c file through a PCH — and letting CMake emit
        # a C-language PCH would compile the C++ stdafx.h as C
        get_target_property(_srcs ${target} SOURCES)
        set(_c_srcs "${_srcs}")
        list(FILTER _c_srcs INCLUDE REGEX "\\.c$")
        if (_c_srcs)
            set_source_files_properties(${_c_srcs} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
        endif()
    endif()
endfunction()

# glob helper: "all .cpp under DIR except EXCLUDE names" (case-insensitive,
# the source tree mixes OccRasterizer/occRasterizer spellings freely)
function(xray_glob out_var dir)
    cmake_parse_arguments(ARG "" "" "EXCLUDE" ${ARGN})
    file(GLOB _files CONFIGURE_DEPENDS "${dir}/*.cpp")
    if (ARG_EXCLUDE)
        string(TOLOWER "${ARG_EXCLUDE}" _excl_lower)
        set(_kept "")
        foreach(_f IN LISTS _files)
            get_filename_component(_name "${_f}" NAME)
            string(TOLOWER "${_name}" _name_lower)
            if (NOT _name_lower IN_LIST _excl_lower)
                list(APPEND _kept "${_f}")
            endif()
        endforeach()
        set(_files "${_kept}")
    endif()
    set(${out_var} "${_files}" PARENT_SCOPE)
endfunction()

# case-insensitive removal from an existing list by file name
function(xray_exclude list_var)
    string(TOLOWER "${ARGN}" _excl_lower)
    set(_kept "")
    foreach(_f IN LISTS ${list_var})
        get_filename_component(_name "${_f}" NAME)
        string(TOLOWER "${_name}" _name_lower)
        if (NOT _name_lower IN_LIST _excl_lower)
            list(APPEND _kept "${_f}")
        endif()
    endforeach()
    set(${list_var} "${_kept}" PARENT_SCOPE)
endfunction()

# Classic MSVC /Yc + /Yu precompiled header, replicating the vcxproj semantics
# exactly: the compiler SKIPS all text up to the `#include "<header>"` line in
# every consumer, so out-of-dir TUs never parse their sibling stdafx.h. This is
# the load-bearing difference from CMake's force-include PCH for targets that
# pull sources from other projects (XrECore, LevelEditor).
function(xray_msvc_pch target pch_name creator)
    cmake_parse_arguments(ARG "" "" "NOPCH" ${ARGN})
    set(_pch "${CMAKE_BINARY_DIR}/pch/${target}.pch")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/pch")
    string(TOLOWER "${ARG_NOPCH}" _nopch_lower)
    get_filename_component(_creator_abs "${creator}" ABSOLUTE)
    get_target_property(_srcs ${target} SOURCES)
    foreach(_f IN LISTS _srcs)
        if (NOT _f MATCHES "\\.(cpp|cxx)$")
            continue()
        endif()
        get_filename_component(_name "${_f}" NAME)
        string(TOLOWER "${_name}" _name_lower)
        get_filename_component(_abs "${_f}" ABSOLUTE)
        # sources shared between targets (e.g. stats_manager.cpp in XrECore and
        # LevelEditor) must pick THIS target's pch only — source properties are
        # directory-global, so gate the flags on the consuming target's name
        set(_guard "$<STREQUAL:$<TARGET_PROPERTY:NAME>,${target}>")
        if (_abs STREQUAL _creator_abs)
            set_property(SOURCE "${_f}" APPEND PROPERTY COMPILE_OPTIONS
                "$<${_guard}:/Yc${pch_name}>" "$<${_guard}:/Fp${_pch}>")
            set_property(SOURCE "${_f}" PROPERTY OBJECT_OUTPUTS "${_pch}")
        elseif (_name_lower IN_LIST _nopch_lower)
            # compiles without any PCH, exactly like PrecompiledHeader=NotUsing
        else()
            set_property(SOURCE "${_f}" APPEND PROPERTY COMPILE_OPTIONS
                "$<${_guard}:/Yu${pch_name}>" "$<${_guard}:/Fp${_pch}>")
            set_property(SOURCE "${_f}" APPEND PROPERTY OBJECT_DEPENDS "${_pch}")
        endif()
    endforeach()
endfunction()

# mark files that must build without the target's PCH
function(xray_no_pch)
    set_source_files_properties(${ARGN} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endfunction()
