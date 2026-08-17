# Engine core DLLs — ported 1:1 from the vcxproj specs (Release|x64).

set(SRC "${XRAY_SOURCE}")

#-- XrAPI ----------------------------------------------------------------------
xray_glob(XRAPI_SRC "${SRC}/XrAPI")
add_library(XrAPI SHARED ${XRAPI_SRC})
xray_common(XrAPI PCH "${SRC}/XrAPI/stdafx.h")
target_compile_definitions(XrAPI PRIVATE XRAPI_EXPORTS _WINDOWS _USRDLL)

#-- XrCore ---------------------------------------------------------------------
xray_glob(XRCORE_SRC "${SRC}/XrCore")
set(XRCORE_C
    "${SRC}/XrCore/ptmalloc3/malloc.c"
    "${SRC}/XrCore/ptmalloc3/sysdeps/win32/win32.c"
)
add_library(XrCore SHARED ${XRCORE_SRC} ${XRCORE_C})
xray_common(XrCore PCH "${SRC}/XrCore/stdafx.h")
target_compile_definitions(XrCore PRIVATE XRCORE_EXPORTS _WINDOWS _USRDLL)
target_link_libraries(XrCore
    PRIVATE XrAPI dxerr comdlg32 LZO
)
target_include_directories(XrCore PUBLIC "${LZO_ROOT}/include")
xray_no_pch(${XRCORE_C} "${SRC}/XrCore/Model.cpp")

#-- XrCDB ----------------------------------------------------------------------
xray_glob(XRCDB_SRC "${SRC}/XrCDB" EXCLUDE cl_raypick.cpp)
add_library(XrCDB SHARED ${XRCDB_SRC})
xray_common(XrCDB PCH "${SRC}/XrCDB/stdafx.h")
target_compile_definitions(XrCDB PRIVATE XRCDB_EXPORTS _WINDOWS _USRDLL)
target_link_libraries(XrCDB PRIVATE XrAPI XrCore winmm)

#-- XrNetServer ----------------------------------------------------------------
xray_glob(XRNET_SRC "${SRC}/XrNetServer")
add_library(XrNetServer SHARED ${XRNET_SRC})
xray_common(XrNetServer PCH "${SRC}/XrNetServer/stdafx.h")
target_compile_definitions(XrNetServer PRIVATE XR_NETSERVER_EXPORTS XRNETSERVER_EXPORTS _WINDOWS _USRDLL)
target_link_libraries(XrNetServer PRIVATE XrCore Ws2_32 dxerr dxguid)

#-- XrXMLParser ----------------------------------------------------------------
xray_glob(XRXML_SRC "${SRC}/XrXMLParser")
add_library(XrXMLParser SHARED ${XRXML_SRC})
xray_common(XrXMLParser PCH "${SRC}/XrXMLParser/stdafx.h")
target_compile_definitions(XrXMLParser PRIVATE XRXMLPARSER_EXPORTS _WINDOWS _USRDLL)
target_link_libraries(XrXMLParser PRIVATE XrAPI XrCore)

#-- XrCPU_Pipe -----------------------------------------------------------------
xray_glob(XRCPU_SRC "${SRC}/XrCPU_Pipe" EXCLUDE xrSkin2W_SSE.cpp)
add_library(XrCPU_Pipe SHARED ${XRCPU_SRC})
xray_common(XrCPU_Pipe PCH "${SRC}/XrCPU_Pipe/stdafx.h")
target_compile_definitions(XrCPU_Pipe PRIVATE XRCPUPIPE_EXPORTS _WINDOWS _USRDLL)
target_link_libraries(XrCPU_Pipe PRIVATE XrCore)

#-- XrParticles ----------------------------------------------------------------
xray_glob(XRPART_SRC "${SRC}/XrParticles")
add_library(XrParticles SHARED ${XRPART_SRC})
xray_common(XrParticles PCH "${SRC}/XrParticles/stdafx.h")
target_compile_definitions(XrParticles PRIVATE XRPARTICLES_EXPORTS _WINDOWS _USRDLL)
target_link_libraries(XrParticles PRIVATE XrAPI XrCore XrCPU_Pipe)

#-- XrSound --------------------------------------------------------------------
xray_glob(XRSND_SRC "${SRC}/XrSound"
    EXCLUDE MusicStream.cpp SoundRender_CoreD.cpp SoundRender_TargetD.cpp xr_cda.cpp xr_streamsnd.cpp)
add_library(XrSound SHARED ${XRSND_SRC})
xray_common(XrSound PCH "${SRC}/XrSound/stdafx.h")
target_compile_definitions(XrSound PRIVATE XRSOUND_EXPORTS _WINDOWS _USRDLL)
target_link_libraries(XrSound PRIVATE Ogg OpenAL Vorbis XrCDB XrCore dxguid)
xray_no_pch("${SRC}/XrSound/guids.cpp")

#-- XrEngine -------------------------------------------------------------------
xray_glob(XRENGINE_SRC "${SRC}/XrEngine"
    EXCLUDE xrLoadSurface.cpp editor_environment_manager_properties.cpp xrTheora_Surface_mmx.cpp)
set(XRENGINE_C "${SRC}/XrEngine/doug_lea_memory_allocator.c")
add_library(XrEngine SHARED ${XRENGINE_SRC} ${XRENGINE_C} "${SRC}/XrEngine/resource.rc")
xray_common(XrEngine PCH "${SRC}/XrEngine/stdafx.h")
target_compile_definitions(XrEngine PRIVATE ENGINE_BUILD _WINDOWS)
target_link_libraries(XrEngine PRIVATE
    Luabind Ogg OpenAutomate Theora XrAPI XrCDB XrCore XrNetServer XrSound
    lua51 vfw32 Winmm dxguid dinput8)
xray_no_pch(${XRENGINE_C})
# stage the prebuilt LuaJIT runtime next to the binaries (was a post-build XCOPY)
add_custom_command(TARGET XrEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${XRAY_ROOT}/SDK/Bin/x64/lua51.dll" "${XRAY_BIN}/lua51.dll")

#-- XrPhysics ------------------------------------------------------------------
xray_glob(XRPHYS_SRC  "${SRC}/XrPhysics")
xray_glob(XRPHYS_DCYL "${SRC}/XrPhysics/dcylinder")
xray_glob(XRPHYS_TRI  "${SRC}/XrPhysics/tri-colliderknoopc" EXCLUDE dcTriListCollider.cpp)
add_library(XrPhysics SHARED
    ${XRPHYS_SRC} ${XRPHYS_DCYL} ${XRPHYS_TRI}
    "${SRC}/xrServerEntities/PHNetState.cpp")
xray_common(XrPhysics PCH "${SRC}/XrPhysics/stdafx.h")
target_compile_definitions(XrPhysics PRIVATE dSINGLE XRPHYSICS_EXPORTS _WINDOWS _USRDLL)
target_include_directories(XrPhysics PRIVATE "${SRC}/XrPhysics")
target_link_libraries(XrPhysics PRIVATE Ode XrAPI XrCDB XrCore XrEngine)

#-- XrGameSpy ------------------------------------------------------------------
xray_glob(XRGS_CPP "${SRC}/XrGameSpy" EXCLUDE GameSpy_Patching.cpp)
# vendored gamespy SDK: exact directory set from the vcxproj, C sources, no PCH
set(XRGS_C_DIRS
    gamespy gamespy/common gamespy/gcdkey gamespy/ghttp gamespy/GP gamespy/gstats
    gamespy/gt2 gamespy/natneg gamespy/pt gamespy/qr2 gamespy/sake gamespy/sc
    gamespy/serverbrowsing gamespy/webservices)
set(XRGS_C "")
foreach(_d IN LISTS XRGS_C_DIRS)
    file(GLOB _c CONFIGURE_DEPENDS "${SRC}/XrGameSpy/${_d}/*.c")
    list(APPEND XRGS_C ${_c})
endforeach()
# excluded-from-build platform stubs
foreach(_e gsAssert.c gsDebug.c gsMemory.c gsPlatform.c gsPlatformSocket.c gsPlatformThread.c gsPlatformUtil.c)
    list(FILTER XRGS_C EXCLUDE REGEX "/common/${_e}$")
endforeach()
add_library(XrGameSpy SHARED ${XRGS_CPP} ${XRGS_C})
xray_common(XrGameSpy PCH "${SRC}/XrGameSpy/stdafx.h")
target_compile_definitions(XrGameSpy PRIVATE XRGAMESPY_EXPORTS XRAY_DISABLE_GAMESPY_WARNINGS _WINDOWS _USRDLL)
xray_no_pch(${XRGS_C})

#-- XrQSlim (static) -----------------------------------------------------------
xray_glob(XRQSLIM_SRC "${SRC}/Editors/XrQSlim")
add_library(XrQSlim STATIC ${XRQSLIM_SRC})
xray_common(XrQSlim NO_MBCS)
target_compile_definitions(XrQSlim PRIVATE _LIB)
