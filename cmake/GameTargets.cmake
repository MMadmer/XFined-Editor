# Game DLLs (CoP / SoC / CS) — the runtime modules the LevelEditor loads.
# Dual-PCH note: the vcxproj builds a second pch_script.h PCH for the script
# TUs. pch_script.h includes stdafx.h itself, so here every TU shares the one
# stdafx.h PCH — semantically identical, only a slower cold build.

set(SRC "${XRAY_SOURCE}")

#-- XrGame (CoP) ---------------------------------------------------------------
file(GLOB_RECURSE XRGAME_SRC CONFIGURE_DEPENDS
    "${SRC}/XrGame/*.cpp" "${SRC}/XrGame/*.c" "${SRC}/XrGame/*.cxx")
# on disk but never in the project / ExcludedFromBuild
foreach(_e
    Artifact.cpp ContextMenu.cpp GameMtlLib.cpp GameMtlLib_Engine.cpp LevelFogOfWar.cpp
    PhysicsShellScript.cpp SleepEffector.cpp smart_cover_manager.cpp stalker_animation_offsets.cpp
    WeaponHUD.cpp WeaponMounted.cpp controller_psy_aura.cpp FractionState.cpp IUWpnParams.cpp
    UILabel.cpp UINewsWnd.cpp UIOutfitInfo2.cpp UIRankFaction.cpp UIStalkersRankingWnd.cpp
    UITreeViewItem.cpp
    ai_rat_fsm.cpp debug_make_final_test.cpp DynamicHeightMap.cpp static_cast_checked_test.cpp
    UIRankFraction.cpp)
    list(FILTER XRGAME_SRC EXCLUDE REGEX "/${_e}$")
endforeach()
file(GLOB XRGAME_SE CONFIGURE_DEPENDS "${SRC}/xrServerEntities/*.cpp")
list(FILTER XRGAME_SE EXCLUDE REGEX "/PHSynchronize\\.cpp$")
set(XRGAME_EXTERNAL
    "${SRC}/External/Public/luabind/LuaMemoryController.cpp"
    "${SRC}/XrGameSpy/gamespy/md5c.c"
    ${XRGAME_SE})

add_library(XrGame SHARED ${XRGAME_SRC} ${XRGAME_EXTERNAL})
xray_common(XrGame PCH "${SRC}/XrGame/stdafx.h")
target_compile_definitions(XrGame PRIVATE XRGAME_EXPORTS dSINGLE _WINDOWS _USRDLL)
target_include_directories(XrGame PRIVATE "${SRC}/XrGame" "${SRC}/xrServerEntities")
target_link_libraries(XrGame PRIVATE
    Crypto Luabind XrAPI XrCDB XrCore XrEngine XrNetServer XrParticles XrPhysics XrSound
    XrXMLParser lua51)
xray_no_pch(
    "${SRC}/XrGameSpy/gamespy/md5c.c"
    "${SRC}/XrGame/atlas_stalkercoppc_v1.c"
    "${SRC}/XrGame/gamespy/CdkeyDecode/base32.c"
    "${SRC}/XrGame/gamespy/CdkeyDecode/cdkeydecode.c")
add_dependencies(XrGame XrGameSpy)  # runtime module, order-only like the sln

#-- XrGameSOC ------------------------------------------------------------------
file(GLOB_RECURSE XRSOC_SRC CONFIGURE_DEPENDS
    "${SRC}/XrGameSOC/*.cpp" "${SRC}/XrGameSOC/*.c" "${SRC}/XrGameSOC/*.cxx")
# whole subtrees never compiled
list(FILTER XRSOC_SRC EXCLUDE REGEX "/xrD3D9-Null/")
list(FILTER XRSOC_SRC EXCLUDE REGEX "/XrGameSOC/xrGameSpy/")
foreach(_e
    GameMtlLib.cpp GameMtlLib_Engine.cpp
    DynamicHeightMap.cpp LevelFogOfWar.cpp dcTriListCollider.cpp UIPointerGage.cpp)
    list(FILTER XRSOC_SRC EXCLUDE REGEX "/${_e}$")
endforeach()
file(GLOB XRSOC_SE CONFIGURE_DEPENDS "${SRC}/XrServerEntitiesSOC/*.cpp" "${SRC}/XrServerEntitiesSOC/*.c")
list(FILTER XRSOC_SE EXCLUDE REGEX "/doug_lea_memory_allocator\\.c$")

add_library(XrGameSOC SHARED ${XRSOC_SRC} ${XRSOC_SE})
xray_common(XrGameSOC PCH "${SRC}/XrGameSOC/stdafx.h")
target_compile_definitions(XrGameSOC PRIVATE XRGAME_EXPORTS dSINGLE XRGAMESOC_EXPORTS _WINDOWS _USRDLL)
target_include_directories(XrGameSOC PRIVATE "${SRC}/XrGameSOC" "${SRC}/XrServerEntitiesSOC")
target_link_libraries(XrGameSOC PRIVATE
    Luabind Ode XrAPI XrCDB XrCore XrEngine XrNetServer XrParticles XrSound XrXMLParser lua51)
xray_no_pch(
    "${SRC}/XrGameSOC/gamespy/cdkeydecode/base32.c"
    "${SRC}/XrGameSOC/gamespy/cdkeydecode/cdkeydecode.c")

#-- XrGameCS -------------------------------------------------------------------
file(GLOB_RECURSE XRCS_SRC CONFIGURE_DEPENDS
    "${SRC}/XrGameCS/*.cpp" "${SRC}/XrGameCS/*.c" "${SRC}/XrGameCS/*.cxx")
foreach(_e
    Artifact.cpp ContextMenu.cpp GameMtlLib.cpp GameMtlLib_Engine.cpp LevelFogOfWar.cpp
    SleepEffector.cpp smart_cover_manager.cpp stalker_animation_offsets.cpp WeaponHUD.cpp
    WeaponMounted.cpp FractionState.cpp IUWpnParams.cpp UIRankFraction.cpp UIVideoPlayerWnd.cpp
    ai_rat_fsm.cpp debug_make_final_test.cpp DynamicHeightMap.cpp static_cast_checked_test.cpp
    dcTriListCollider.cpp UIEventsWnd.cpp UIInventoryWnd.cpp uiinventorywnd2.cpp
    UIInventoryWnd3.cpp UIOutfitInfo2.cpp UIOutfitSlot.cpp UITaskDescrWnd.cpp UITaskItem.cpp)
    list(FILTER XRCS_SRC EXCLUDE REGEX "/${_e}$")
endforeach()
file(GLOB XRCS_SE CONFIGURE_DEPENDS "${SRC}/XrServerEntitiesCS/*.cpp" "${SRC}/XrServerEntitiesCS/*.c")

add_library(XrGameCS SHARED ${XRCS_SRC} ${XRCS_SE} "${SRC}/XrGameSpy/gamespy/md5c.c")
xray_common(XrGameCS NO_MBCS PCH "${SRC}/XrGameCS/stdafx.h")  # CharacterSet NotSet
target_compile_definitions(XrGameCS PRIVATE XRGAME_EXPORTS dSINGLE XRGAMECS_EXPORTS _WINDOWS _USRDLL)
target_include_directories(XrGameCS PRIVATE "${SRC}/XrGameCS" "${SRC}/XrServerEntitiesCS")
# XrNetServer is missing from the vcxproj refs because XrGameCS was never
# actually built by the old solution (ActiveCfg without Build.0) — the MP code
# plainly imports IPureServer/IPureClient from it
target_link_libraries(XrGameCS PRIVATE
    Crypto Luabind Ode XrAPI XrCDB XrCore XrEngine XrNetServer XrSound XrXMLParser lua51)
xray_no_pch(
    "${SRC}/XrGameSpy/gamespy/md5c.c"
    "${SRC}/XrServerEntitiesCS/doug_lea_memory_allocator.c"
    "${SRC}/XrGameCS/gamespy/cdkeydecode/base32.c"
    "${SRC}/XrGameCS/gamespy/cdkeydecode/cdkeydecode.c")
