# Third-party libraries under Source/External — ported from vcxproj specs.
# All import Source/Default.props (so they go through xray_common) except the
# RedImage subtree, which has its own minimal property sheet.

set(SRC "${XRAY_SOURCE}")
set(EXT "${SRC}/External")

#-- simple static libs ---------------------------------------------------------
xray_glob(LUABIND_SRC "${EXT}/Luabind")
add_library(Luabind STATIC ${LUABIND_SRC})
xray_common(Luabind PCH "${EXT}/Luabind/pch.h")
target_compile_definitions(Luabind PRIVATE _LIB)

add_library(Ogg STATIC "${EXT}/Ogg/bitwise.c" "${EXT}/Ogg/framing.c")
xray_common(Ogg)
target_compile_definitions(Ogg PRIVATE _LIB)

file(GLOB VORBIS_SRC CONFIGURE_DEPENDS "${EXT}/Vorbis/*.c" "${EXT}/Vorbis/*.cpp")
add_library(Vorbis STATIC ${VORBIS_SRC})
xray_common(Vorbis)
target_compile_definitions(Vorbis PRIVATE _LIB)

file(GLOB THEORA_SRC CONFIGURE_DEPENDS "${EXT}/Theora/*.c" "${EXT}/Theora/*.cpp")
add_library(Theora STATIC ${THEORA_SRC})
xray_common(Theora)
target_compile_definitions(Theora PRIVATE _LIB)
target_compile_options(Theora PRIVATE /permissive-)

file(GLOB ODE_SRC CONFIGURE_DEPENDS "${EXT}/Ode/*.c" "${EXT}/Ode/*.cpp")
add_library(Ode STATIC ${ODE_SRC})
xray_common(Ode)
target_compile_definitions(Ode PRIVATE dSINGLE WINDOWS ODE_EXPORTS _WINDOWS _USRDLL)

file(GLOB CRYPTO_SRC CONFIGURE_DEPENDS "${EXT}/Crypto/*.c" "${EXT}/Crypto/*.cpp"
    "${EXT}/Crypto/openssl/src/*.c")
add_library(Crypto STATIC ${CRYPTO_SRC})
xray_common(Crypto)
target_compile_definitions(Crypto PRIVATE _LIB)
target_include_directories(Crypto PRIVATE "${EXT}/Crypto/openssl")

add_library(OpenAutomate STATIC "${EXT}/OpenAutomate/OpenAutomate.c")
xray_common(OpenAutomate)
target_compile_definitions(OpenAutomate PRIVATE _LIB)

# zlib: the one project without /arch:AVX — configured by hand for parity
file(GLOB ZLIB_SRC CONFIGURE_DEPENDS "${EXT}/zlib/*.c")
add_library(zlib STATIC ${ZLIB_SRC})
set_target_properties(zlib PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${XRAY_LIB}")
target_compile_definitions(zlib PRIVATE
    _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS _WINDLL _SECURE_SCL=0 _ITERATOR_DEBUG_LEVEL=0
    $<$<CONFIG:Release>:DEBUG> $<$<CONFIG:Release>:NDEBUG>)
target_include_directories(zlib PRIVATE "${EXT}/Public")  # headers live in External/Public/zlib/
target_compile_options(zlib PRIVATE /W0 /GR)

#-- OpenAL (dll) ---------------------------------------------------------------
add_library(OpenAL SHARED
    "${EXT}/OpenAL/al.cpp" "${EXT}/OpenAL/alc.cpp"
    "${EXT}/OpenAL/alList.cpp" "${EXT}/OpenAL/OpenAL32.cpp")
xray_common(OpenAL)
target_compile_definitions(OpenAL PRIVATE AL_BUILD_LIBRARY OPENAL_EXPORTS _WINDOWS _USRDLL)
target_link_libraries(OpenAL PRIVATE version winmm odbc32 odbccp32)

#-- RedImage subtree (own property sheet: no engine defines, own includes) -----
set(REDIMAGE "${EXT}/RedImage")

function(redimage_common target)
    set_target_properties(${target} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${XRAY_LIB}"
        CXX_STANDARD 14)
    target_compile_definitions(${target} PRIVATE _ITERATOR_DEBUG_LEVEL=0)
    target_include_directories(${target} PRIVATE "${REDIMAGE}")
    # note: no /Oi here — nvtt's nvmath.h defines log2f inline, which C2169s
    # against CRT intrinsics; only ispc_texcomp had IntrinsicFunctions on
    target_compile_options(${target} PRIVATE /W0)
endfunction()

# ispc_texcomp: two C++ TUs + ten ISPC-compiled objects (checked-in ispc.exe)
set(ISPC_EXE "${REDIMAGE}/ISPC/win/ispc.exe")
set(ISPC_DIR "${REDIMAGE}/ispc_texcomp")
set(ISPC_OUT "${CMAKE_BINARY_DIR}/ispc")
file(MAKE_DIRECTORY "${ISPC_OUT}")
set(ISPC_OBJS "")
foreach(_k kernel kernel_astc)
    set(_objs
        "${ISPC_OUT}/${_k}.obj"      "${ISPC_OUT}/${_k}_sse2.obj" "${ISPC_OUT}/${_k}_sse4.obj"
        "${ISPC_OUT}/${_k}_avx.obj"  "${ISPC_OUT}/${_k}_avx2.obj")
    add_custom_command(
        OUTPUT ${_objs}
        COMMAND "${ISPC_EXE}" -O2 "${_k}.ispc"
                -o "${ISPC_OUT}/${_k}.obj"
                -h "${ISPC_DIR}/${_k}_ispc.h"
                --target=sse2,sse4,avx,avx2 --opt=fast-math
        WORKING_DIRECTORY "${ISPC_DIR}"
        MAIN_DEPENDENCY "${ISPC_DIR}/${_k}.ispc"
        COMMENT "ISPC ${_k}.ispc")
    list(APPEND ISPC_OBJS ${_objs})
endforeach()
set_source_files_properties(${ISPC_OBJS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_library(ispc_texcomp STATIC
    "${ISPC_DIR}/ispc_texcomp.cpp" "${ISPC_DIR}/ispc_texcomp_astc.cpp" ${ISPC_OBJS})
redimage_common(ispc_texcomp)
target_compile_definitions(ispc_texcomp PRIVATE _MBCS)
target_compile_options(ispc_texcomp PRIVATE /Oi /Gy)

# nvtt
file(GLOB NVTT_SRC CONFIGURE_DEPENDS
    "${REDIMAGE}/nvtt/bc6h/*.cpp" "${REDIMAGE}/nvtt/bc7/*.cpp" "${REDIMAGE}/nvtt/cuda/*.cpp"
    "${REDIMAGE}/nvtt/nvcore/*.cpp" "${REDIMAGE}/nvtt/nvimage/*.cpp" "${REDIMAGE}/nvtt/nvmath/*.cpp"
    "${REDIMAGE}/nvtt/nvthread/*.cpp" "${REDIMAGE}/nvtt/nvtt/*.cpp" "${REDIMAGE}/nvtt/squish/*.cpp")
add_library(nvtt STATIC ${NVTT_SRC})
redimage_common(nvtt)
target_include_directories(nvtt PRIVATE "${REDIMAGE}/nvtt")

# RedImageTool (Unicode, PCH RedImage.hpp)
file(GLOB REDIMAGETOOL_SRC CONFIGURE_DEPENDS "${REDIMAGE}/RedImageTool/*.cpp")
add_library(RedImageTool STATIC ${REDIMAGETOOL_SRC})
redimage_common(RedImageTool)
target_compile_definitions(RedImageTool PRIVATE _LIB _UNICODE UNICODE)
target_include_directories(RedImageTool PRIVATE "${REDIMAGE}/nvtt")
target_precompile_headers(RedImageTool PRIVATE "${REDIMAGE}/RedImageTool/RedImage.hpp")
target_link_libraries(RedImageTool PUBLIC ispc_texcomp nvtt)
