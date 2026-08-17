#include "stdafx.h"
#include "..\..\XrECore\Editor\EditorModManifest.h"
#include "..\Editor\EditorModScene.h"
UIMainMenuForm::UIMainMenuForm()
{
}

UIMainMenuForm::~UIMainMenuForm()
{
}

void UIMainMenuForm::Draw()
{
  
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Clear", "")) { ExecCommand(COMMAND_CLEAR); }
            if (ImGui::MenuItem("Open...", "")) { ExecCommand(COMMAND_LOAD); }
            if (ImGui::MenuItem("Save", "")) { ExecCommand(COMMAND_SAVE, xr_string(LTools->m_LastFileName.c_str())); }
            if (ImGui::MenuItem("Save As ...", "")) { ExecCommand(COMMAND_SAVE, 0,1); }
            ImGui::Separator();
            if (ImGui::MenuItem("Open Selection...", "")) { ExecCommand(COMMAND_LOAD_SELECTION); }
            if (ImGui::MenuItem("Save Selection As...", "")) { ExecCommand(COMMAND_SAVE_SELECTION); }
            ImGui::Separator();
            if (ImGui::BeginMenu("Open Recent", ""))
            {
                for (auto& str : EPrefs->scene_recent_list)
                {
                    if (ImGui::MenuItem(str.c_str(), "")) { ExecCommand(COMMAND_LOAD, str); }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Project"))
            {
                ImGui::TextDisabled("Project: %s", EditorProject::Name());
                ImGui::Separator();
                if (ImGui::MenuItem("Import Base Scene...", ""))	EditorProject::RequestImportScene();
                if (ImGui::MenuItem("Open Project Folder", ""))		EditorProject::OpenProjectFolder();
                ImGui::Separator();
                if (ImGui::MenuItem("Switch Project...", ""))
                {
                    // Ask BEFORE closing anything: COMMAND_CLEAR's result was
                    // ignored here, so cancelling the "scene has been modified"
                    // prompt still closed the project and took the scene with
                    // it. IfModified() answers false only on Cancel, and by the
                    // time CLEAR asks again the flag is already settled.
                    if (Scene->IfModified())
                    {
                        ExecCommand(COMMAND_CLEAR);
                        EditorProject::Close();
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "")) { ExecCommand(COMMAND_QUIT); }
            ImGui::EndMenu();
        }

        // Mod is the whole point of this editor, so it is a menu of its own -
        // buried under File nobody found it, and "Compile" next door is the
        // level geometry compiler, which has nothing to do with shipping a mod.
        if (ImGui::BeginMenu("Mod"))
        {
            char build_target[MAX_PATH] = {};
            EditorMod::BuildTargetText(build_target, sizeof(build_target));

            ImGui::BeginDisabled(!EditorMod::CanBuildIntoGame());
            if (ImGui::MenuItem("Build Mod into Game", "Ctrl+B"))	EditorMod::RequestBuildIntoGame();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(build_target[0]
                    ? "Clean build straight into the linked game:\n%s\n\nThe folder is mirrored - files the project\nno longer has are removed."
                    : "Link a game and set a valid module id first.%s",
                    build_target);

            if (ImGui::MenuItem("Build to Another Folder...", ""))	EditorMod::RequestExportModule();
            if (ImGui::MenuItem("Export Flat Gamedata...", ""))		EditorMod::RequestExportFlat();
            ImGui::Separator();
            if (ImGui::MenuItem("Edit Manifest...", ""))			EditorMod::RequestEditManifest();
            ImGui::Separator();
            // objects placed before this flag existed (or intentionally traded
            // between mod and base) are re-labelled here; the build ships only
            // mod content, an imported base level stays the game's
            if (ImGui::MenuItem("Mark Selection as Mod Content", ""))
            {
                int n = 0;
                ObjectList sel;
                if (Scene && Scene->GetQueryObjects(sel, OBJCLASS_DUMMY, 1, -1, -1))
                    for (ObjectIt it = sel.begin(); it != sel.end(); ++it)
                        if (!(*it)->IsAuthorPlaced())
                            { (*it)->SetAuthorPlaced(TRUE); ++n; }
                if (n) Scene->UndoSave();
                ELog.Msg(mtInformation, "Mod content: %d object(s) marked.", n);
            }
            if (ImGui::MenuItem("Mark Selection as Base Level", ""))
            {
                int n = 0;
                ObjectList sel;
                if (Scene && Scene->GetQueryObjects(sel, OBJCLASS_DUMMY, 1, -1, -1))
                    for (ObjectIt it = sel.begin(); it != sel.end(); ++it)
                        if ((*it)->IsAuthorPlaced())
                            { (*it)->SetAuthorPlaced(FALSE); ++n; }
                if (n) Scene->UndoSave();
                ELog.Msg(mtInformation, "Base level: %d object(s) unmarked.", n);
            }
            ImGui::Separator();
            // level deltas baked from the current selection: spawn ops first,
            // they are the composable way to put something on a base level
            if (ImGui::BeginMenu("Bake Scene Layer"))
            {
                if (ImGui::MenuItem("Spawn Layer...", ""))			EditorModScene::RequestExportSpawn();
                // level overlays baked from the current selection (sector 0)
                if (ImGui::MenuItem("Collision Overlay...", ""))		EditorModScene::RequestExportXCForm();
                if (ImGui::MenuItem("Visual Overlay...", ""))		EditorModScene::RequestExportOGF();
                // selection used as volumes: removes base collision + visuals
                if (ImGui::MenuItem("Level Cut...", ""))			EditorModScene::RequestExportCut();
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Scene"))
        {
            if (ImGui::MenuItem("Validate", "")) { ExecCommand(COMMAND_VALIDATE_SCENE); }
            ImGui::Separator();
            if (ImGui::MenuItem("Summary Info", "")) {
                ExecCommand(COMMAND_CLEAR_SCENE_SUMMARY);
                ExecCommand(COMMAND_COLLECT_SCENE_SUMMARY);
                ExecCommand(COMMAND_SHOW_SCENE_SUMMARY);
            }
            if (ImGui::MenuItem("Highlight Texture...", ""))
            {
                ExecCommand(COMMAND_SCENE_HIGHLIGHT_TEXTURE);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Debug Draw", ""))
            {
                ExecCommand(COMMAND_CLEAR_DEBUG_DRAW);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export entire Scene as Obj", ""))
            {
                Scene->ExportObj(false);
            }
            if (ImGui::MenuItem("Export selection as Obj", ""))
            {
                Scene->ExportObj(true);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Compile"))
		{
            if (ImGui::BeginMenu("Make"))
            {
                if (ImGui::MenuItem("Make All", ""))
                {
                    ExecCommand(COMMAND_BUILD);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Make Game", ""))
                {
                    ExecCommand(COMMAND_MAKE_GAME);
                }
                if (ImGui::MenuItem("Make Details", ""))
                {
                    ExecCommand(COMMAND_MAKE_DETAILS);
                }
                if (ImGui::MenuItem("Make Hom", ""))
                {
                    ExecCommand(COMMAND_MAKE_HOM);
                }
                if (ImGui::MenuItem("Make SOM", ""))
                {
                    ExecCommand(COMMAND_MAKE_SOM);
                }
                if (ImGui::MenuItem("Make AI-Map", ""))
                {
                    ExecCommand(COMMAND_MAKE_AIMAP);
                }
                ImGui::EndMenu();
            }
            bool bDisable = false;
			if (LTools->IsCompilerRunning() || LTools->IsGameRunning())
			{
				ImGui::BeginDisabled();
                bDisable = true;
			}
            if (ImGui::BeginMenu("Compile"))
			{
				if (ImGui::MenuItem("Geometry & Light", ""))
				{
                    LTools->RunXrLC();
				}
				if (ImGui::MenuItem("Detail Object Light", ""))
				{
					LTools->RunXrDO();
				}
                if (ImGui::BeginMenu("AIMap"))
				{
					if (ImGui::MenuItem("High", ""))
					{
						LTools->RunXrAI_AIMap(false);
					}
					if (ImGui::MenuItem("Low", ""))
					{
						LTools->RunXrAI_AIMap(false);
					}
					if (ImGui::MenuItem("Verify", ""))
					{
                        LTools->RunXrAI_Verify();
					}
					ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Spawn"))
				{
					if (ImGui::MenuItem("Only current level", ""))
					{
						LTools->RunXrAI_Spawn(true);
					}
					if (ImGui::MenuItem("All levels", ""))
					{
						LTools->RunXrAI_Spawn(false);
					}
					ImGui::EndMenu();
                }
				
                ImGui::EndMenu();
            }
			if (bDisable)
			{
				ImGui::EndDisabled();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Import Error List", ""))
			{
				ExecCommand(COMMAND_IMPORT_COMPILER_ERROR);
			}
			if (ImGui::MenuItem("Export Error List", ""))
			{
				ExecCommand(COMMAND_EXPORT_COMPILER_ERROR);
			}
			if (ImGui::MenuItem("Clear Error List", ""))
			{
				ExecCommand(COMMAND_CLEAR_DEBUG_DRAW);
			}
			ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Editors"))
        {
            if (ImGui::BeginMenu("Objects"))
            {
                if (ImGui::MenuItem("Reload")) { ExecCommand(COMMAND_RELOAD_OBJECTS); }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Images"))
            {
                if (ImGui::MenuItem("Image Editor", "")) { ExecCommand(COMMAND_IMAGE_EDITOR); }
                ImGui::Separator();
                if (ImGui::MenuItem("Reload Textures", "")) { ExecCommand(COMMAND_RELOAD_TEXTURES); }
                ImGui::Separator();
                if (ImGui::MenuItem("Synchronize Textures", "")) { ExecCommand(COMMAND_REFRESH_TEXTURES); }
                if (ImGui::MenuItem("Cheack New Textures", "")) { ExecCommand(COMMAND_CHECK_TEXTURES); }
                ImGui::Separator();
                if (ImGui::MenuItem("Sync THM", ""))
                {
                    FS_FileSet      files;
                    FS.file_list(files, _textures_, FS_ListFiles, "*.thm");
                    FS_FileSet::iterator I = files.begin();
                    FS_FileSet::iterator E = files.end();

                    // rewrites every .thm in the texture library in place - the
                    // neighbouring Synchronize commands ask, and this one moves
                    // far more files than either
                    if (mrYes != ELog.DlgMsg(mtConfirmation, mbYes | mbNo,
                        "Rewrite all %d .thm file(s) in the texture library?", int(files.size())))
                        files.clear();

                    for (I = files.begin(), E = files.end(); I != E; ++I)
                    {
                        ETextureThumbnail* TH = xr_new<ETextureThumbnail>((*I).name.c_str(), false);
                        TH->Load((*I).name.c_str(), _textures_);
                        TH->Save();
                        xr_delete(TH);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Sounds"))
            {
                if (ImGui::MenuItem("Sound Editor", "")) { ExecCommand(COMMAND_SOUND_EDITOR); }
                ImGui::Separator();
                if (ImGui::MenuItem("Synchronize Sounds", "")) { ExecCommand(COMMAND_SYNC_SOUNDS); }
                ImGui::Separator();
                if (ImGui::MenuItem("Refresh Environment Library", "")) { ExecCommand(COMMAND_REFRESH_SOUND_ENVS); }
                if (ImGui::MenuItem("Refresh Environment Geometry", "")) { ExecCommand(COMMAND_REFRESH_SOUND_ENV_GEOMETRY); }
                ImGui::EndMenu();

            }
            if (ImGui::MenuItem("Light Anim Editor", "")) { ExecCommand(COMMAND_LIGHTANIM_EDITOR); }
            if (ImGui::MenuItem("Minimap Editor", "")) { ExecCommand(COMMAND_MINIMAP_EDITOR); }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Options"))
        {
            {
                bool any_class = !!Tools->GetSettings(etfPickAnyClass);
                if (ImGui::MenuItem("Pick Any Class", "", &any_class))
                    ExecCommand(COMMAND_SET_SETTINGS, etfPickAnyClass, any_class);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Unreal-style picking: click selects an object of any class\nand switches the active target to it");
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Render"))
            {
                if (ImGui::BeginMenu("GPU"))
                {
                    ImGui::TextDisabled("Active: %s", GetActiveGpuName());
                    ImGui::Separator();

                    // Real adapters, by name, as the runtime reports them - the
                    // device is created on the one picked here. The driver-level
                    // preference below only nudges Windows and cannot pick a
                    // specific card, which is why both exist.
                    CLevelPreferences* prefs = dynamic_cast<CLevelPreferences*>(EPrefs);
                    {
                        bool is_default = !prefs || prefs->GpuAdapter.empty();
                        if (ImGui::MenuItem("System default", "", &is_default) && is_default && prefs)
                            prefs->GpuAdapter = "";
                    }
                    const int gpu_count = GpuAdapterCount();
                    for (int i = 0; i < gpu_count; ++i)
                    {
                        LPCSTR name = GpuAdapterName(i);
                        bool selected = prefs && (0 == xr_strcmp(prefs->GpuAdapter.c_str(), name));
                        if (ImGui::MenuItem(name, "", &selected) && selected && prefs)
                            prefs->GpuAdapter = name;
                    }
                    if (!gpu_count)
                        ImGui::TextDisabled("no adapters reported");

                    ImGui::Separator();
                    const EGpuPreference current = GetGpuPreference();
                    struct { EGpuPreference pref; const char* title; } items[] = {
                        { gpuAuto,         "Driver hint: auto" },
                        { gpuPerformance,  "Driver hint: high performance" },
                        { gpuPowerSaving,  "Driver hint: power saving" },
                    };
                    for (auto& it : items)
                    {
                        bool selected = (current == it.pref);
                        if (ImGui::MenuItem(it.title, "", &selected) && selected)
                            SetGpuPreference(it.pref);
                    }

                    ImGui::Separator();
                    ImGui::TextDisabled("Takes effect after restart");
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Quality"))
                {
                    static bool selected[4] = { false,false,true,false };
                    if (ImGui::MenuItem("25%", "", &selected[0]))
                    {
                        selected[1] = selected[2] = selected[3] = false;
                        UI->SetRenderQuality(1 / 4.f);
                        UI->RedrawScene();
                    }
                    if (ImGui::MenuItem("50%", "", &selected[1]))
                    {
                        selected[0] = selected[2] = selected[3] = false;
                        UI->SetRenderQuality(1 / 2.f);
                        UI->RedrawScene();
                    }
                    if (ImGui::MenuItem("100%", "", &selected[2]))
                    {
                        selected[1] = selected[0] = selected[3] = false;
                        UI->SetRenderQuality(1.f);
                        UI->RedrawScene();
                    }
                    if (ImGui::MenuItem("200%", "", &selected[3]))
                    {
                        selected[1] = selected[2] = selected[0] = false;
                        UI->SetRenderQuality(2.f);
                        UI->RedrawScene();
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Fill Mode"))
                {
                    bool selected[3] = { EDevice->dwFillMode == D3DFILL_POINT,EDevice->dwFillMode == D3DFILL_WIREFRAME,EDevice->dwFillMode == D3DFILL_SOLID };
                    if (ImGui::MenuItem("Point", "", &selected[0]))
                    {
                        EDevice->dwFillMode = D3DFILL_POINT;
                        UI->RedrawScene();
                    }
                    if (ImGui::MenuItem("Wireframe", "", &selected[1]))
                    {
                        EDevice->dwFillMode = D3DFILL_WIREFRAME;
                        UI->RedrawScene();
                    }
                    if (ImGui::MenuItem("Solid", "", &selected[2]))
                    {
                        EDevice->dwFillMode = D3DFILL_SOLID;
                        UI->RedrawScene();
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Shader Mode"))
                {
                    bool selected[2] = { EDevice->dwShadeMode == D3DSHADE_FLAT,EDevice->dwShadeMode == D3DSHADE_GOURAUD };
                    if (ImGui::MenuItem("Flat", "", &selected[0]))
                    {
                        EDevice->dwShadeMode = D3DSHADE_FLAT;
                        UI->RedrawScene();
                    }
                    if (ImGui::MenuItem("Gouraud", "", &selected[1]))
                    {
                        EDevice->dwShadeMode = D3DSHADE_GOURAUD;
                        UI->RedrawScene();
                    }
                    ImGui::EndMenu();
                }
                {
                    bool selected = psDeviceFlags.test(rsEdgedFaces);
                    if (ImGui::MenuItem("Edged Faces", "", &selected))
                    {
                        psDeviceFlags.set(rsEdgedFaces, selected);
                        UI->RedrawScene();
                    }
                }
                ImGui::Separator();
                {
                    bool selected = !HW.Caps.bForceGPU_SW;
                    if (ImGui::MenuItem("RenderHW", "", &selected))
                    {
                        HW.Caps.bForceGPU_SW = !selected;
                        UI->Resize();
                    }
                }
                ImGui::Separator();
                {
                    bool selected = psDeviceFlags.test(rsFilterLinear);
                    if (ImGui::MenuItem("Filter Linear", "", &selected))
                    {
                        psDeviceFlags.set(rsFilterLinear, selected);
                        UI->RedrawScene();
                    }
                }
                {
                    bool selected = psDeviceFlags.test(rsRenderTextures);
                    if (ImGui::MenuItem("Textures", "", &selected))
                    {
                        psDeviceFlags.set(rsRenderTextures, selected);
                        UI->RedrawScene();
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            {
                bool selected = psDeviceFlags.test(rsDrawSafeRect);
                if (ImGui::MenuItem("Draw Safe Rect", "", &selected))
                {
                    psDeviceFlags.set(rsDrawSafeRect, selected);
                    UI->RedrawScene();
                }
            }
            {
                bool selected = psDeviceFlags.test(rsDrawGrid);
                if (ImGui::MenuItem("Draw Grid", "", &selected))
                {
                    psDeviceFlags.set(rsDrawGrid, selected);
                    UI->RedrawScene();
                }
            }
            ImGui::Separator();
            {
                bool selected = psDeviceFlags.test(rsFog);
                if (ImGui::MenuItem("Fog", "", &selected))
                {
                    psDeviceFlags.set(rsFog, selected);
                    UI->RedrawScene();
                }
            }
            {
                if (ImGui::BeginMenu("Environment"))
                {
                    bool selected = !psDeviceFlags.test(rsEnvironment);
                    if (ImGui::MenuItem("None", "", &selected))
                    {
                        psDeviceFlags.set(rsEnvironment, false);
                        UI->RedrawScene();
                    }
                    ImGui::Separator();
                    for (auto& i : g_pGamePersistent->Environment().WeatherCycles)
                    {
                        selected = psDeviceFlags.test(rsEnvironment) && i.first == g_pGamePersistent->Environment().CurrentCycleName;
                        if (ImGui::MenuItem(i.first.c_str(), "", &selected))
                        {
                            psDeviceFlags.set(rsEnvironment, true);
                            g_pGamePersistent->Environment().SetWeather(i.first.c_str(),true);
                            UI->RedrawScene();
                        }
                    }
                   
                    ImGui::EndMenu();
                }
            }
            ImGui::Separator();
            {
                bool selected = psDeviceFlags.test(rsLighting);;
                if (ImGui::MenuItem("Lighting", "", &selected))
                {
                    psDeviceFlags.set(rsLighting, selected);
                    UI->RedrawScene();
                }
            }
            {
                bool selected = psDeviceFlags.test(rsMuteSounds);
                if (ImGui::MenuItem("Mute Sounds", "", &selected))
                {
                    psDeviceFlags.set(rsMuteSounds, selected);
                }
            }
            {
                bool selected = psDeviceFlags.test(rsRenderRealTime);
                if (ImGui::MenuItem("Real Time", "", &selected))
                {
                    psDeviceFlags.set(rsRenderRealTime, selected);
                }
            }
            ImGui::Separator();
            {
                bool selected = psDeviceFlags.test(rsStatistic);
                if (ImGui::MenuItem("Stats", "",&selected)) { psDeviceFlags.set(rsStatistic, selected);  UI->RedrawScene(); }

            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Theme"))
            {
                for (u32 i = 0; i < u32(XFinedTheme::Preset::Count); ++i)
                {
                    const XFinedTheme::Preset preset = XFinedTheme::Preset(i);
                    const bool selected = EPrefs->ui_theme_preset == i;
                    if (ImGui::MenuItem(XFinedTheme::Name(preset), "", selected))
                        EPrefs->SetThemePreset(i);
                }
                ImGui::Separator();
                const u32 default_preset = u32(XFinedTheme::Default());
                ImGui::BeginDisabled(EPrefs->ui_theme_preset == default_preset);
                if (ImGui::MenuItem("Reset to Default"))
                    EPrefs->SetThemePreset(default_preset);
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences", "")) { ExecCommand(COMMAND_EDITOR_PREF); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows"))
        {
            if (ImGui::MenuItem("Command Palette", "Ctrl+Shift+P"))
                ExecCommand(COMMAND_COMMAND_PALETTE);
            ImGui::Separator();
            {
                bool selected = UIContentBrowser::IsOpen();
                if (ImGui::MenuItem("Content Browser", "", &selected)) { if (selected) UIContentBrowser::Show(); else UIContentBrowser::Close(); }
            }
            {
                bool selected = UIWorldOutliner::IsOpen();
                if (ImGui::MenuItem("World Outliner", "", &selected)) { if (selected) UIWorldOutliner::Show(); else UIWorldOutliner::Close(); }
            }
            {
                bool selected = UIObjectList::IsOpen();
                if (ImGui::MenuItem("Object List", "", &selected)) { if (selected) UIObjectList::Show(); else UIObjectList::Close(); }
            }
            {
                bool selected = UIVisualPreview::IsOpen();
                if (ImGui::MenuItem("Model Preview", "", &selected)) { if (selected) UIVisualPreview::Show(); else UIVisualPreview::Close(); }
            }
            {
                // the texture viewer only has something to show once an image is
                // double-clicked, so the menu can close it but not open it empty
                bool selected = UIImagePreview::IsOpen();
                if (ImGui::MenuItem("Texture Preview", "", &selected, selected) && !selected)
                    UIImagePreview::Close();
            }
            {
                bool selected = !MainForm->GetPropertiesFrom()->IsClosed();
                if (ImGui::MenuItem("Properties", "", &selected)) { if (selected)MainForm->GetPropertiesFrom()->Open(); else MainForm->GetPropertiesFrom()->Close(); }
            }
			{
				bool selected = !MainForm->GetWorldPropertiesFrom()->IsClosed();
				if (ImGui::MenuItem("World Properties", "", &selected)) { if (selected)MainForm->GetWorldPropertiesFrom()->Open(); else MainForm->GetWorldPropertiesFrom()->Close(); }
			}
            {
                bool selected = AllowLogCommands();

                if (ImGui::MenuItem("Log", "",&selected)) { ExecCommand(COMMAND_LOG_COMMANDS); }
            }
            ImGui::Separator();
            // rebuilds the Unreal-style arrangement and forgets manual docking
            if (ImGui::MenuItem("Reset Layout", "")) { UI->RequestDockLayoutReset(); }

           ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

