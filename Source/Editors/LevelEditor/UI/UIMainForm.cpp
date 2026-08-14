#include "stdafx.h"
#include "..\XrECore\Editor\EditorChooseEvents.h"
#include "..\..\XrECore\Editor\EditorGameConfigs.h"
#include "..\..\XrECore\Editor\EditorModManifest.h"
#include "..\Editor\EditorModScene.h"
#include "..\Editor\Utils\XrSEFactoryManager.h"
#include "..\Editor\Tools\Spawn\ESceneSpawnTools.h"

#if defined(USE_DX11)
// ImGui binds a shader resource view under D3D11; surface_get() returns the
// resource behind it, which the backend cannot bind.
IC ImTextureID ui_imtex(ref_texture& t) { return t->get_SRView(); }
#else
IC ImTextureID ui_imtex(ref_texture& t) { return t->surface_get(); }
#endif

UIMainForm* MainForm = nullptr;
UIMainForm::UIMainForm()
{
    EnableReceiveCommands();
    if (!ExecCommand(COMMAND_INITIALIZE, (u32)0, (u32)0))
    {
        FlushLog();
        exit(-1);
    }
    ExecCommand(COMMAND_UPDATE_GRID);
    ExecCommand(COMMAND_RENDER_FOCUS);
    FillChooseEvents();
    m_TopBar = xr_new<UITopBarForm>();
    m_Render = xr_new<UIRenderForm>();
    m_MainMenu = xr_new<UIMainMenuForm>();
    m_LeftBar = xr_new<UILeftBarForm>();
    m_Properties = xr_new<UILPropertiesFrom>();
    m_WorldProperties = xr_new<UIWorldPropertiesFrom>();
    m_Render->SetContextMenuEvent(TOnRenderContextMenu(this, &UIMainForm::DrawContextMenu));
    m_Render->SetToolBarEvent(TOnRenderToolBar(this, &UIMainForm::DrawRenderToolBar));
    m_Render->SetDropAssetEvent(TOnRenderDropAsset(this, &UIMainForm::DropAsset));
    // every build path mirrors the open scene into its spawn layer first -
    // "save scene, press Build" must ship what the viewport shows
    EditorMod::SetPreBuildBake(&EditorModScene::BakeCurrentSceneSpawn);
    if (dynamic_cast<CLevelPreferences*>(EPrefs)->OpenObjectList)
    {
        UIObjectList::Show();
    }
    if (dynamic_cast<CLevelPreferences*>(EPrefs)->OpenContentBrowser)
    {
        UIContentBrowser::Show();
    }
    if (dynamic_cast<CLevelPreferences*>(EPrefs)->OpenWorldOutliner)
    {
        UIWorldOutliner::Show();
    }
    if (!dynamic_cast<CLevelPreferences*>(EPrefs)->OpenProperties)
    {
        m_Properties->Close();
    }
	if (!dynamic_cast<CLevelPreferences*>(EPrefs)->OpenWorldProperties)
	{
        m_WorldProperties->Close();
	}

    m_tMenu = EDevice->Resources->_CreateTexture("ed\\bar\\menu");
    m_tSelect = EDevice->Resources->_CreateTexture("ed\\bar\\select");
    m_tAdd = EDevice->Resources->_CreateTexture("ed\\bar\\add");
    m_tMove = EDevice->Resources->_CreateTexture("ed\\bar\\move");
    m_tScale = EDevice->Resources->_CreateTexture("ed\\bar\\scale");
    m_tRotate = EDevice->Resources->_CreateTexture("ed\\bar\\rotate");
    m_tNSnap = EDevice->Resources->_CreateTexture("ed\\bar\\nsnap");
    m_tZoomSel = EDevice->Resources->_CreateTexture("ed\\bar\\zoomsel");

    m_tGrid = EDevice->Resources->_CreateTexture("ed\\bar\\grid");
    m_tScaleGrid = EDevice->Resources->_CreateTexture("ed\\bar\\scale_grid");
    m_tAngle = EDevice->Resources->_CreateTexture("ed\\bar\\angle");

    extern bool XFinedInspector(LPCSTR, LPCSTR, xr_string&);
    XFinedMCP::SetHandler(XFinedInspector);
    XFinedMCP::Start();

    LTools->GetGimzo()->SetStep(Gizmo::EType::Move, 0.1f);
    LTools->GetGimzo()->SetStep(Gizmo::EType::Scale, 0.1f);
    LTools->GetGimzo()->SetStep(Gizmo::EType::Rotate, 5.625f);

    LTools->GetGimzo()->SwitchStep(Gizmo::EType::Move, true);
    LTools->GetGimzo()->SwitchStep(Gizmo::EType::Scale, true);
    LTools->GetGimzo()->SwitchStep(Gizmo::EType::Rotate, true);

}

UIMainForm::~UIMainForm()
{
	dynamic_cast<CLevelPreferences*>(EPrefs)->OpenProperties = !m_Properties->IsClosed();
	dynamic_cast<CLevelPreferences*>(EPrefs)->OpenWorldProperties = !m_WorldProperties->IsClosed();
    dynamic_cast<CLevelPreferences*>(EPrefs)->OpenObjectList = UIObjectList::IsOpen();
    dynamic_cast<CLevelPreferences*>(EPrefs)->OpenContentBrowser = UIContentBrowser::IsOpen();
    dynamic_cast<CLevelPreferences*>(EPrefs)->OpenWorldOutliner = UIWorldOutliner::IsOpen();
    XFinedMCP::Stop();
    EditorProject::Close();		// preview + manifest while the device is alive
    UIContentBrowser::Close();
    UIWorldOutliner::Close();
    ClearChooseEvents();
    xr_delete(m_WorldProperties);
    xr_delete(m_Properties);
    xr_delete(m_LeftBar);
    xr_delete(m_MainMenu);
    xr_delete(m_Render);
    xr_delete(m_TopBar);
    
    m_tMenu.destroy();
    m_tSelect.destroy();
    m_tAdd.destroy();
    m_tMove.destroy();
    m_tScale.destroy();
    m_tRotate.destroy();
    m_tNSnap.destroy();
    m_tZoomSel.destroy();
    m_tGrid.destroy();
    m_tScaleGrid.destroy();
    m_tAngle.destroy();

    ExecCommand(COMMAND_DESTROY, (u32)0, (u32)0);
}

void UIMainForm::DropAsset(LPCSTR name)
{
    // The payload is the browser's own name; placement needs a library ref.
    // The translation depends on which source the browser is showing.
    string_path ref;
    if (UIContentBrowser::ResolveDropRef(name, ref))
    {
        // p2 bit0 -> place under the cursor, the ray was refreshed by the drop target
        ExecCommand(COMMAND_CB_PLACE_ASSET, xr_string(ref), u32(1));
        return;
    }
    // not an editable reference - but a compiled model still places, as a
    // raw-visual scene object (bit1 picks that loader in the command)
    if (UIContentBrowser::ResolveDropVisual(name, ref))
    {
        ExecCommand(COMMAND_CB_PLACE_ASSET, xr_string(ref), u32(1|2));
        return;
    }
    ELog.Msg(mtError, "'%s' cannot be placed in the scene - only .object references and .ogf models can.", name);
}

// Swap pSettings for the linked game's configs, then rebuild everything that
// cached the old ones: the server-entity factory (its statics parse actor
// profiles out of pSettings) and the Spawn tool's class roster. Runs once
// per link fingerprint; scene entities are not touched, because this fires
// before the pending scene of a fresh link ever loads.
static void SyncGameConfigs()
{
    static xr_string s_active;
    if (!EditorProject::Active() || !EditorProject::GameLinked())
        return;
    if (s_active.size() && s_active == EditorGameConfigs::ActiveFingerprint())
        return;

    xr_string err;
    if (!EditorGameConfigs::Activate(err))
    {
        static bool s_said = false;
        if (!s_said)
        {
            Msg("! game configs: %s - the editor keeps its bundled ones", err.c_str());
            s_said = true;
        }
        return;
    }

    // the factory's initialize() re-reads $game_config$, which now points at
    // the extracted tree; its destroy() also frees the pSettings Activate
    // just built, and initialize() creates the final one
    xr_delete(g_SEFactoryManager);
    g_SEFactoryManager = xr_new<XrSEFactoryManager>();

    if (ESceneSpawnTool* tool = Scene ? dynamic_cast<ESceneSpawnTool*>(Scene->GetTool(OBJCLASS_SPAWNPOINT)) : 0)
        tool->RefreshClasses();

    s_active = EditorGameConfigs::ActiveFingerprint();
}

void UIMainForm::Draw()
{
    bOpen = true;
    // UE-style: until a project is opened, the browser page is the whole UI
    // (the MCP pump lives in TUI::Idle so it runs for background windows too)
    if (EditorProject::DrawBrowser())
        return;
    // a project with no valid game link blocks the editor exactly the same way
    if (EditorProject::DrawGameLink())
        return;
    // The game is linked from here on - make its configs the editor's own.
    // Order matters: this must happen BEFORE the pending scene loads, or the
    // scene's spawn entities would be built by a factory reading the wrong
    // game's sections. Fingerprinted, so every later frame is a no-op.
    SyncGameConfigs();
    // deferred last-scene autoload — first clean frame after the browser closed
    if (LPCSTR pending = EditorProject::PopPendingScene())
        ExecCommand(COMMAND_LOAD, xr_string(pending));
    m_MainMenu->Draw();
    m_TopBar->Draw();
    m_LeftBar->Draw();
	m_Properties->Draw();
	m_WorldProperties->Draw();
    m_Render->Draw();
}

bool UIMainForm::Frame()
{
    if(UI)  return UI->Idle();
    return false;
}

void UIMainForm::DrawContextMenu()
{
    if (ImGui::BeginMenu("Edit"))
    {
        if (ImGui::MenuItem("Copy"))
        {
            ExecCommand(COMMAND_COPY);
        }
        if (ImGui::MenuItem("Paste"))
        {
            ExecCommand(COMMAND_PASTE);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut"))
        {
            ExecCommand(COMMAND_CUT);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete"))
        {
            ExecCommand(COMMAND_DELETE_SELECTION);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Visiblity"))
    {
        if (ImGui::MenuItem("Hide Selected"))
        {
            ExecCommand(COMMAND_HIDE_SEL, FALSE);
        }
        if (ImGui::MenuItem("Hide Unselected"))
        {
            ExecCommand(COMMAND_HIDE_UNSEL);
        }
        if (ImGui::MenuItem("Hide All"))
        {
            ExecCommand(COMMAND_HIDE_ALL, FALSE);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Unhide All"))
        {
            ExecCommand(COMMAND_HIDE_ALL, TRUE);
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Properties"))
    {
        ExecCommand(COMMAND_SHOW_PROPERTIES);
    }
   
   
}

void UIMainForm::DrawRenderToolBar(ImVec2 Size)
{
    {
        ImGui::BeginGroup();
        m_tMenu->Load();
        {
			if (ImGui::BeginPopupContextItem("MenuScene"))
			{
				
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
								g_pGamePersistent->Environment().SetWeather(i.first.c_str(), true);
								UI->RedrawScene();
							}
						}

						ImGui::EndMenu();
					}
				}
				ImGui::Separator();
				if (ImGui::BeginMenu("Render"))
				{
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
					{
						bool selected = psDeviceFlags.test(rsEdgedFaces);
						if (ImGui::MenuItem("Edged Faces", "", &selected))
						{
							psDeviceFlags.set(rsEdgedFaces, selected);
							UI->RedrawScene();
						}
					}
					{
						bool selected = psDeviceFlags.test(rsLighting);;
						if (ImGui::MenuItem("Lighting", "", &selected))
						{
							psDeviceFlags.set(rsLighting, selected);
							UI->RedrawScene();
						}
					}
					ImGui::EndMenu();
				}
				ImGui::Separator();
				
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
					if (ImGui::MenuItem("Stats", "", &selected)) { psDeviceFlags.set(rsStatistic, selected);  UI->RedrawScene(); }

				}
				ImGui::EndPopup();
			}
            if (ImGui::ImageButton(ui_imtex(m_tMenu), ImVec2(16, ImGui::GetFontSize())))
			{
				ImGui::OpenPopup("MenuScene");
            }
        }
        ImGui::EndGroup();
    }

	bool bIsSupportMove = false;
	bool bIsSupportRotate = false;
	bool bIsSupportScale = false;
    {
        ETAction Action = LTools->GetAction();

    
        if (Scene->GetTool(LTools->CurrentClassID())&& Scene->GetTool(LTools->CurrentClassID())->FindControl(0,etaSelect))
        {
          auto Control=   Scene->GetTool(LTools->CurrentClassID())->FindControl(0, etaSelect);
		  bIsSupportMove = Control->IsSupportMove();
          bIsSupportRotate = Control->IsSupportRotate();
          bIsSupportScale = Control->IsSupportScale();
        }
		if (!bIsSupportMove&&Action == etaSelect && LTools->GetGimzo()->GetType() == Gizmo::EType::Move)
		{
            LTools->GetGimzo()->SetType(Gizmo::EType::None);
        }
		if (!bIsSupportRotate && Action == etaSelect && LTools->GetGimzo()->GetType() == Gizmo::EType::Rotate)
		{
			LTools->GetGimzo()->SetType(Gizmo::EType::None);
		}
		if (!bIsSupportScale && Action == etaSelect && LTools->GetGimzo()->GetType() == Gizmo::EType::Scale)
		{
			LTools->GetGimzo()->SetType(Gizmo::EType::None);
		}

        ImGui::SameLine(0, ImGui::GetFontSize() * 5);
        ImGui::BeginGroup();


        m_tSelect->Load();
        {
            bool bPushColor = false;
            if (Action == etaSelect&&LTools->GetGimzo()->GetType() == Gizmo::EType::None)
            {
                bPushColor = true;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Border));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Border));
            }
            if (ImGui::ImageButton(ui_imtex(m_tSelect), ImVec2(16, ImGui::GetFontSize())))
            {
                LTools->SetAction(etaSelect);
                LTools->GetGimzo()->SetType(Gizmo::EType::None);
            }
            if (bPushColor)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
            }
        }

        ImGui::SameLine();

        {
            bool bPushColor = false;
            if (Action==etaAdd)
            {
                bPushColor = true;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Border));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Border));
            }
            m_tAdd->Load();
            if (ImGui::ImageButton(ui_imtex(m_tAdd), ImVec2(16, ImGui::GetFontSize())))
            {
                LTools->SetAction(etaAdd);
                LTools->GetGimzo()->SetType(Gizmo::EType::None);
            }
            if (bPushColor)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
            }
        }
     

        ImGui::SameLine();
        {

            bool bPushColor = false;

			ImGui::BeginDisabled(!bIsSupportMove);
            if (Action == etaSelect && LTools->GetGimzo()->GetType() == Gizmo::EType::Move)
            {
                bPushColor = true;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Border));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Border));
            }
            m_tMove->Load();
            if (ImGui::ImageButton(ui_imtex(m_tMove), ImVec2(16, ImGui::GetFontSize())))
            {
                LTools->SetAction(etaSelect);
                LTools->GetGimzo()->SetType(Gizmo::EType::Move);
            }

            if (bPushColor)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
			}
			ImGui::EndDisabled();
        }
        

        ImGui::SameLine();
        {
            bool bPushColor = false;
            ImGui::BeginDisabled(!bIsSupportScale);
            if (Action == etaSelect && LTools->GetGimzo()->GetType() == Gizmo::EType::Scale)
            {
                bPushColor = true;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Border));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Border));
            }
            m_tScale->Load();
            if (ImGui::ImageButton(ui_imtex(m_tScale), ImVec2(16, ImGui::GetFontSize())))
            {
                LTools->SetAction(etaSelect);
                LTools->GetGimzo()->SetType(Gizmo::EType::Scale);
            }
            if (bPushColor)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
            }
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        { 
            bool bPushColor = false;

			ImGui::BeginDisabled(!bIsSupportRotate);
            if (Action == etaSelect && LTools->GetGimzo()->GetType() == Gizmo::EType::Rotate)
            {
                bPushColor = true;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Border));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Border));
            }
            m_tRotate->Load();
            if (ImGui::ImageButton(ui_imtex(m_tRotate), ImVec2(16, ImGui::GetFontSize())))
            {
                LTools->SetAction(etaSelect);
                LTools->GetGimzo()->SetType(Gizmo::EType::Rotate);
            }
            if (bPushColor)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
            }

			ImGui::EndDisabled();
        }

        ImGui::EndGroup();
    }
    {
        ImGui::SameLine(0, ImGui::GetFontSize() * 5);
        ImGui::BeginGroup();

        {
            bool bPushColor = false;
            if (Tools->GetSettings(etfNormalAlign))
            {
                bPushColor = true;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Border));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Border));
            }
            m_tNSnap->Load();
            if (ImGui::ImageButton(ui_imtex(m_tNSnap), ImVec2(16, ImGui::GetFontSize()), ImVec2(0, 0), ImVec2(0.5f, 1.f)))
            {
                ExecCommand(COMMAND_SET_SETTINGS, etfNormalAlign, !Tools->GetSettings(etfNormalAlign));
            }
			if (bPushColor)
			{
				ImGui::PopStyleColor();
				ImGui::PopStyleColor();
			}
        }

        ImGui::SameLine();

        m_tZoomSel->Load();
        if (ImGui::ImageButton(ui_imtex(m_tZoomSel), ImVec2(16, ImGui::GetFontSize()),ImVec2(0,0), ImVec2(0.5f, 1.f)))
		{
			ExecCommand(COMMAND_ZOOM_EXTENTS, TRUE);
        }
        ImGui::EndGroup();
    }
    {
        string_path Temp;
        ImGui::SameLine(0, ImGui::GetFontSize() * 5);
        ImGui::BeginGroup();

        {
            bool bPushColor = false;

            if (LTools->GetGimzo()->IsStepEnable(Gizmo::EType::Move))
            {
                bPushColor = true;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Border));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Border));
            }
            m_tGrid->Load();
            if (ImGui::ImageButton(ui_imtex(m_tGrid), ImVec2(16, ImGui::GetFontSize())))
            {
                LTools->GetGimzo()->SwitchStep(Gizmo::EType::Move, !LTools->GetGimzo()->IsStepEnable(Gizmo::EType::Move));
            }
            if (bPushColor)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
            }

        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        xr_sprintf(Temp, "%.2f", LTools->GetGimzo()->GetStep(Gizmo::EType::Move));
        if (ImGui::BeginCombo("##move", Temp, ImGuiComboFlags_None))
        {
            if (ImGui::Selectable("0.01", false)) 
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Move, 0.01f);
            }
            if (ImGui::Selectable("0.05", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Move, 0.05f);
            }
            if (ImGui::Selectable("0.1", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Move, 0.1f);
            }
            if (ImGui::Selectable("1", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Move, 1.f);
            }
            if (ImGui::Selectable("2", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Move, 2.f);
            }
            if (ImGui::Selectable("5", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Move, 5.f);
            }
            if (ImGui::Selectable("50", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Move, 50.f);
            }
            if (ImGui::Selectable("200", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Move, 200.f);
            }
            if (ImGui::Selectable("1000", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Move, 1000.f);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, ImGui::GetFontSize());

        {
            bool bPushColor = false;
            if (LTools->GetGimzo()->IsStepEnable(Gizmo::EType::Scale))
            {
                bPushColor = true;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Border));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Border));
            }
            m_tScaleGrid->Load();
            if (ImGui::ImageButton(ui_imtex(m_tScaleGrid), ImVec2(16, ImGui::GetFontSize())))
            {
                LTools->GetGimzo()->SwitchStep(Gizmo::EType::Scale, !LTools->GetGimzo()->IsStepEnable(Gizmo::EType::Scale));
            }
            if (bPushColor)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
            }
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        xr_sprintf(Temp, "%.2f", LTools->GetGimzo()->GetStep(Gizmo::EType::Scale));
        if (ImGui::BeginCombo("##scale", Temp, ImGuiComboFlags_None))
        {
            if (ImGui::Selectable("0.01", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Scale, 0.01f);
            }
            if (ImGui::Selectable("0.05", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Scale, 0.05f);
            }
            if (ImGui::Selectable("0.1", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Scale, 0.1f);
            }
            if (ImGui::Selectable("1", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Scale, 1.f);
            }
            if (ImGui::Selectable("2", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Scale, 2.f);
            }
            if (ImGui::Selectable("5", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Scale, 5.f);
            }
            if (ImGui::Selectable("50", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Scale, 50.f);
            }
            if (ImGui::Selectable("200", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Scale, 200.f);
            }
            if (ImGui::Selectable("1000", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Scale, 1000.f);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, ImGui::GetFontSize());

        {
            bool bPushColor = false;
            if (LTools->GetGimzo()->IsStepEnable(Gizmo::EType::Rotate))
            {
                bPushColor = true;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Border));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Border));
            }
            m_tAngle->Load();
            if (ImGui::ImageButton(ui_imtex(m_tAngle), ImVec2(16, ImGui::GetFontSize())))
            {
                LTools->GetGimzo()->SwitchStep(Gizmo::EType::Rotate, !LTools->GetGimzo()->IsStepEnable(Gizmo::EType::Rotate));
            }
            if (bPushColor)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleColor();
            }
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        xr_sprintf(Temp, "%.5f", LTools->GetGimzo()->GetStep(Gizmo::EType::Rotate));
        if (ImGui::BeginCombo("##rotate", Temp, ImGuiComboFlags_None))
        {
            if (ImGui::Selectable("1.40625", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Rotate, 1.40625f);
            }
            if (ImGui::Selectable("2.8125", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Rotate, 2.8125f);
            }
            if (ImGui::Selectable("5.625", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Rotate, 5.625f);
            }
            if (ImGui::Selectable("11.25", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Rotate, 11.25f);
            }
            if (ImGui::Selectable("22.5", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Rotate, 22.5f);
            }
            if (ImGui::Selectable("45", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Rotate, 45.f);
            }
            if (ImGui::Selectable("90", false))
            {
                LTools->GetGimzo()->SetStep(Gizmo::EType::Rotate, 90.f);
            }
            ImGui::EndCombo();
        }
        ImGui::EndGroup();
    }


}
