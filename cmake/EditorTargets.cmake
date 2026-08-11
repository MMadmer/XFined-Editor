# Editor DLLs and the LevelEditor — ported from vcxproj specs verified against
# the MSBuild tlog command lines. Release|x64 parity.

set(SRC "${XRAY_SOURCE}")
set(ED  "${SRC}/Editors")

#-- FreeMagic (static) ---------------------------------------------------------
xray_glob(FREEMAGIC_SRC "${ED}/FreeMagic")
add_library(FreeMagic STATIC ${FREEMAGIC_SRC})
xray_common(FreeMagic NO_MBCS)
target_compile_definitions(FreeMagic PRIVATE FREEMAGIC_EXPORTS _WINDOWS _USRDLL)

#-- XrEUI ----------------------------------------------------------------------
xray_glob(XREUI_SRC "${ED}/XrEUI")
add_library(XrEUI SHARED ${XREUI_SRC})
xray_common(XrEUI NO_MBCS)  # Unicode project
target_compile_definitions(XrEUI PRIVATE XREUI_EXPORTS _WINDOWS _USRDLL _UNICODE UNICODE)
target_link_libraries(XrEUI PRIVATE XrCore)
target_link_options(XrEUI PRIVATE "/NATVIS:${ED}/XrEUI/imgui.natvis")

#-- XrEProps -------------------------------------------------------------------
file(GLOB XREPROPS_SRC CONFIGURE_DEPENDS
    "${ED}/XrEProps/*.cpp"
    "${ED}/XrEProps/Tree/Base/*.cpp"
    "${ED}/XrEProps/Tree/Choose/*.cpp"
    "${ED}/XrEProps/Tree/ItemList/*.cpp"
    "${ED}/XrEProps/Tree/Properties/*.cpp")
add_library(XrEProps SHARED ${XREPROPS_SRC})
xray_common(XrEProps NO_MBCS PCH "${ED}/XrEProps/stdafx.h")  # CharacterSet not set
target_compile_definitions(XrEProps PRIVATE XREPROPS_EXPORTS _WINDOWS _USRDLL)
target_include_directories(XrEProps PRIVATE "${ED}/XrEProps")
target_link_libraries(XrEProps PRIVATE XrCore XrEngine XrEUI)

#-- XrETools -------------------------------------------------------------------
xray_glob(XRETOOLS_SRC "${ED}/XrETools")
add_library(XrETools SHARED ${XRETOOLS_SRC})
xray_common(XrETools NO_MBCS PCH "${ED}/XrETools/stdafx.h")  # CharacterSet NotSet
target_compile_definitions(XrETools PRIVATE XRETOOLS_EXPORTS _WINDOWS _USRDLL)
target_link_libraries(XrETools PRIVATE Ogg Vorbis XrCDB XrCore XrQSlim d3d9 d3dx9 dxerr)

#-- XrDXT ----------------------------------------------------------------------
xray_glob(XRDXT_SRC "${ED}/XrDXT" EXCLUDE nvdxt.cpp)
add_library(XrDXT SHARED ${XRDXT_SRC})
xray_common(XrDXT PCH "${ED}/XrDXT/stdafx.h")
target_compile_definitions(XrDXT PRIVATE XRDXT_EXPORTS _WINDOWS _USRDLL)
target_link_libraries(XrDXT PRIVATE RedImageTool XrCore)

#-- XrECore --------------------------------------------------------------------
# in-project trees
xray_glob(XRECORE_EDITOR "${ED}/XrECore/Editor" EXCLUDE bone.cpp)
xray_glob(XRECORE_ENGINE "${ED}/XrECore/Engine"
    EXCLUDE cl_collector.cpp GameMtlLib.cpp GameMtlLib_Engine.cpp)
xray_glob(XRECORE_WM     "${ED}/XrECore/WildMagic")
xray_glob(XRECORE_ROOT   "${ED}/XrECore")
# pulled-in engine/render sources (compiled against XrECore's own PCH context)
set(XRECORE_PRIVATE_EXCLUDE
    ColorMapManager D3DUtils DetailManager DetailManager_CACHE DetailManager_Decompress
    DetailManager_soft DetailManager_VS du_box du_cone du_cylinder du_sphere du_sphere_part
    FLOD FTreeVisual HOM Light Light_DB Light_Package LightTrack NvTriStrip NvTriStripObjects
    OccRasterizer OccRasterizer_core r__dsgraph_build r__dsgraph_render r__dsgraph_render_lods
    r__occlusion r__pixel_calculator r__screenshot r__sector r__sector_traversal uber_deffer
    VertexCache WallmarksEngine xr_effgamma xrStripify)
xray_glob(XRECORE_RPRIV "${SRC}/XrRender/Private")
set(_priv_excl "")
foreach(_e IN LISTS XRECORE_PRIVATE_EXCLUDE)
    list(APPEND _priv_excl "${_e}.cpp")
endforeach()
xray_exclude(XRECORE_RPRIV ${_priv_excl})
xray_glob(XRECORE_RBLEND "${SRC}/XrRender/Private/blenders")
set(XRECORE_R1
    BlenderDefault Blender_Blur Blender_default_aref Blender_LaEmB Blender_Model
    Blender_Screen_GRAY Blender_Shadow_World Blender_Vertex Blender_Vertex_aref)
set(XRECORE_R1_SRC "")
foreach(_f IN LISTS XRECORE_R1)
    list(APPEND XRECORE_R1_SRC "${SRC}/XrRender/R1_PC/${_f}.cpp")
endforeach()
set(XRECORE_EXTERNAL
    "${SRC}/XrCPU_Pipe/xrSkin2W.cpp"
    "${SRC}/XrEngine/ai_script_lua_debug.cpp"
    "${SRC}/XrEngine/ai_script_lua_extension.cpp"
    "${SRC}/XrRender/DX9/dx9r_constants_cache.cpp"
    ${XRECORE_RBLEND} ${XRECORE_RPRIV} ${XRECORE_R1_SRC})

add_library(XrECore SHARED
    ${XRECORE_ROOT} ${XRECORE_EDITOR} ${XRECORE_ENGINE} ${XRECORE_WM} ${XRECORE_EXTERNAL})
xray_common(XrECore)
# classic /Yc//Yu: the 90 pulled-in XrRender/XrEngine TUs must have their own
# `#include "stdafx.h"` line SKIPPED, not resolved to the sibling render PCH
xray_msvc_pch(XrECore "stdafx.h" "${ED}/XrECore/stdafx.cpp" NOPCH
    guid.cpp WmlMatrix2.cpp WmlMatrix4.cpp WmlVector2.cpp WmlVector4.cpp)
target_compile_definitions(XrECore PRIVATE XRECORE_EXPORTS _WINDOWS _USRDLL)
target_include_directories(XrECore PRIVATE
    "${ED}/XrECore" "${SRC}/XrEngine" "${SRC}/XrRender/Private" "${SRC}/XrRender/Public"
    "${ED}/FreeMagic")
target_link_libraries(XrECore PRIVATE
    Luabind Ogg RedImageTool Theora Vorbis XrAPI XrCDB XrCore XrEngine XrParticles
    XrPhysics XrSound FreeMagic XrDXT XrEProps XrETools XrEUI
    d3d9 d3dx9 vfw32 Winmm dxguid dinput8 Rpcrt4 lua51)

#-- LevelEditor ----------------------------------------------------------------
file(GLOB_RECURSE LE_SRC CONFIGURE_DEPENDS "${ED}/LevelEditor/*.cpp")
set(LE_EXTERNAL
    "${SRC}/XrRender/Private/DetailManager.cpp"
    "${SRC}/XrRender/Private/DetailManager_CACHE.cpp"
    "${SRC}/XrRender/Private/DetailManager_Decompress.cpp"
    "${SRC}/XrRender/Private/DetailManager_soft.cpp"
    "${SRC}/XrRender/Private/DetailManager_VS.cpp"
    "${SRC}/XrRender/Private/stats_manager.cpp")
add_executable(LevelEditor WIN32 ${LE_SRC} ${LE_EXTERNAL})
xray_common(LevelEditor)
xray_msvc_pch(LevelEditor "stdafx.h" "${ED}/LevelEditor/stdafx.cpp")
target_compile_definitions(LevelEditor PRIVATE _WINDOWS)
target_include_directories(LevelEditor PRIVATE
    "${ED}/LevelEditor" "${SRC}/XrEngine" "${SRC}/XrRender/Public" "${SRC}/XrRender/Private"
    "${ED}/LevelEditor/Engine")
target_link_libraries(LevelEditor PRIVATE
    XrAPI XrCDB XrCore XrEngine XrPhysics XrSound
    FreeMagic XrECore XrEProps XrETools XrEUI
    d3dx9)  # content browser loads project image previews via D3DX

#-- XrSE_Factory family --------------------------------------------------------
# Each factory has TWO precompiled headers in the vcxproj (stdafx.h + pch_script.h).
# The script-side TUs open with pch_script.h themselves, so they are compiled
# without the target PCH instead of replicating the second PCH.
function(xray_se_factory name se_dir)
    cmake_parse_arguments(ARG "" "" "SCRIPT_TUS" ${ARGN})
    set(dir "${ED}/${name}")
    xray_glob(_local "${dir}")
    xray_glob(_se "${SRC}/${se_dir}")
    add_library(${name} SHARED ${_local} ${_se})
    xray_common(${name} PCH "${dir}/stdafx.h")
    target_compile_definitions(${name} PRIVATE XRSEFACTORY_EXPORTS _WINDOWS _USRDLL)
    target_include_directories(${name} PRIVATE "${dir}" "${SRC}/${se_dir}")
    target_link_libraries(${name} PRIVATE Luabind XrCore XrEngine XrXMLParser XrEProps lua51)
    # script-PCH TUs: skip the target PCH, they include pch_script.h themselves
    set(_skip "")
    foreach(_f IN LISTS ARG_SCRIPT_TUS)
        if (EXISTS "${SRC}/${se_dir}/${_f}.cpp")
            list(APPEND _skip "${SRC}/${se_dir}/${_f}.cpp")
        elseif (EXISTS "${dir}/${_f}.cpp")
            list(APPEND _skip "${dir}/${_f}.cpp")
        endif()
    endforeach()
    if (_skip)
        set_source_files_properties(${_skip} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${XRAY_ROOT}/SDK/Bin/x64/lua51.dll" "${XRAY_BIN}/lua51.dll")
endfunction()

set(SE_SCRIPT_COMMON
    pch_script lua_studio object_factory_register object_factory_script object_item_script
    script_engine script_engine_export script_engine_script script_fcolor_script
    script_flags_script script_fmatrix_script script_fvector_script script_ini_file_script
    script_lua_helper script_net_packet_script script_process script_reader_script
    script_rtoken_list_script script_sound_type_script script_stack_tracker script_storage
    script_thread script_token_list_script
    xrServer_Objects_ALife_Items_script xrServer_Objects_ALife_Items_script2
    xrServer_Objects_ALife_Items_script3
    xrServer_Objects_ALife_Monsters_script xrServer_Objects_ALife_Monsters_script2
    xrserver_objects_alife_monsters_script3 xrServer_Objects_ALife_Monsters_script4
    xrServer_Objects_ALife_script xrServer_Objects_ALife_script2 xrServer_Objects_ALife_script3
    xrServer_Objects_script xrServer_Objects_script2)

xray_se_factory(XrSE_Factory xrServerEntities SCRIPT_TUS
    ${SE_SCRIPT_COMMON} xrServer_Objects_Alife_Smartcovers
    xrServer_Objects_Alife_Smartcovers_script xrSE_Factory)
xray_se_factory(XrSE_FactoryCS xrServerEntitiesCS SCRIPT_TUS
    ${SE_SCRIPT_COMMON} xrSE_Factory)
xray_se_factory(XrSE_FactorySOC xrServerEntitiesSOC SCRIPT_TUS ${SE_SCRIPT_COMMON})
target_link_libraries(XrSE_FactorySOC PRIVATE XrAPI)

#-- level compilers (optional: not part of the default working set) ------------
xray_glob(XRLCLIGHT_SRC "${ED}/XrLCLight"
    EXCLUDE net_light.cpp net_task.cpp net_task_manager.cpp stdafx.cpp)
add_library(XrLCLight SHARED EXCLUDE_FROM_ALL
    ${XRLCLIGHT_SRC}
    "${SRC}/XrEngine/xrLoadSurface.cpp"
    "${ED}/Public/hxGridInterface.cpp")
xray_common(XrLCLight)  # no PCH by design
target_compile_definitions(XrLCLight PRIVATE XRLC_LIGHT_EXPORTS _WINDOWS)
target_link_libraries(XrLCLight PRIVATE RedImageTool zlib XrCDB XrCore FreeMagic XrDXT)

set(XRLC_EXCLUDE
    cl_collector.cpp "Copy of xrDeflector.cpp" xrMergeLM.cpp
    Image.cpp OGF_CalculateTB2002.cpp xrDetailPatch.cpp xrFlex2LOD.cpp xrHierrarhy.cpp
    xrMU_Model_export_cform_game.cpp xrMU_Model_export_cform_game_s.cpp xrOptimizeCFORM.cpp
    xrPhase_TangentBasis2002.cpp xrPVS.cpp xrResolveMaterials.cpp xrSoftenLights.cpp
    xrTesselate.cpp xrUVmapping.cpp xrVis.cpp)
xray_glob(XRLC_SRC "${ED}/XrLC" EXCLUDE ${XRLC_EXCLUDE})
xray_glob(XRLC_NVM "${ED}/XrLC/NvMender2002")
xray_glob(XRLC_NVL "${ED}/XrLC/NV_Library")
add_executable(XrLC WIN32 EXCLUDE_FROM_ALL
    ${XRLC_SRC} ${XRLC_NVM} ${XRLC_NVL}
    "${ED}/Public/NVMeshMenderTwo.cpp"
    "${ED}/XrLC/resource.rc")
xray_common(XrLC)  # vcxproj creates a PCH nobody consumes — port without one
target_compile_definitions(XrLC PRIVATE _WINDOWS)
target_include_directories(XrLC PRIVATE "${ED}/XrLC")
target_link_libraries(XrLC PRIVATE
    RedImageTool zlib XrCDB XrCore FreeMagic XrDXT XrETools XrLCLight XrQSlim
    d3dx9 Winmm Comctl32)

xray_glob(XRDO_SRC "${ED}/xrDO_Light")
add_executable(xrDO_Light WIN32 EXCLUDE_FROM_ALL
    ${XRDO_SRC}
    "${ED}/xrDO_Light/resource.rc")
xray_common(xrDO_Light PCH "${ED}/xrDO_Light/StdAfx.h")
target_compile_definitions(xrDO_Light PRIVATE _WINDOWS)
target_link_libraries(xrDO_Light PRIVATE XrCore XrLCLight d3dx9 Winmm Comctl32)
