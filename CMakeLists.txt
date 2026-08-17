# XFined Editor — the single build pipeline (CMake + Ninja).
# Clean clone -> .\Build.ps1 -> Bin\x64\Release with the full working set:
# LevelEditor + engine DLLs + XrGame/SOC/CS + SE factories.
#
# Ported from the old Engine.sln vcxproj files (Release|x64), including their
# quirks (DEBUG defined in Release, /W0, /arch:AVX, C++14). The specs live in
# cmake/*.cmake; the vcxproj files are no longer used by this pipeline.

cmake_minimum_required(VERSION 3.25)
project(XFinedEditor LANGUAGES C CXX RC)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")   # /MD everywhere, like the sln

# parity: the x64 vcxproj configs deliberately do NOT define WIN32 — strip the
# default CMake/MSVC platform define before it poisons legacy #ifdef WIN32 code
foreach(_lang C CXX)
    string(REPLACE "/DWIN32" "" CMAKE_${_lang}_FLAGS "${CMAKE_${_lang}_FLAGS}")
    string(REPLACE "-DWIN32" "" CMAKE_${_lang}_FLAGS "${CMAKE_${_lang}_FLAGS}")
endforeach()

# broken-on-master editors are ported but excluded from the default build;
# the C++/CLI weather editor needs the .NET SDK and is loaded at runtime only
option(XRAY_BUILD_WEATHER_EDITOR "Build the C++/CLI weather editor DLLs" OFF)

include(cmake/XRayCommon.cmake)

# Keep the notices beside every produced editor binary, including targeted builds.
file(MAKE_DIRECTORY "${XRAY_BIN}")
configure_file(
    "${XRAY_SOURCE}/External/THIRD_PARTY_NOTICES.txt"
    "${XRAY_BIN}/THIRD_PARTY_NOTICES.txt"
    COPYONLY
)

include(cmake/ExternalTargets.cmake)
include(cmake/LzoTarget.cmake)
include(cmake/CoreTargets.cmake)
include(cmake/EditorTargets.cmake)
include(cmake/GameTargets.cmake)

# solution-level (order-only) dependencies: runtime-loaded modules must exist
add_dependencies(XrEngine XrCPU_Pipe)
add_dependencies(LevelEditor XrSE_Factory XrSE_FactoryCS XrSE_FactorySOC XrGame XrGameSOC XrGameCS)
