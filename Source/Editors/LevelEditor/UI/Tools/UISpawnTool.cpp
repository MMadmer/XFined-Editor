#include "stdafx.h"

UISpawnTool::UISpawnTool()
{
    m_selPercent = 100;
    m_Current = nullptr;
    m_SpawnList = xr_new<UIItemListForm>();
    m_SpawnList->SetOnItemFocusedEvent(TOnILItemFocused(this, &UISpawnTool::OnItemFocused));
    RefreshList();
    m_AttachObject = false;
}

UISpawnTool::~UISpawnTool()
{
    xr_delete(m_SpawnList);
}

void UISpawnTool::Draw()
{
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::TreeNode("Reference Select"))
    {
        ImGui::Unindent(ImGui::GetStyle().IndentSpacing);
        {
            ImGui::Text("Select by Current: "); ImGui::SameLine(); if (ImGui::Button(" +")) { SelByRefObject(true); } ImGui::SameLine(); if (ImGui::Button(" -")) { SelByRefObject(false); }
            ImGui::Text("Select by Selected:"); ImGui::SameLine(); if (ImGui::Button("=%")) { MultiSelByRefObject(true); } ImGui::SameLine(); if (ImGui::Button("+%")) { MultiSelByRefObject(false); } ImGui::SameLine(); ImGui::SetNextItemWidth(-ImGui::GetTextLineHeight() - 8); ImGui::DragFloat("%", &m_selPercent, 1, 0, 100, "%.1f");
        }
        ImGui::Separator();
        ImGui::Indent(ImGui::GetStyle().IndentSpacing);
        ImGui::TreePop();
    } 
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::TreeNode("Commands"))
    {
        ImGui::Unindent(ImGui::GetStyle().IndentSpacing);
        {
            float size = float(ImGui::CalcItemWidth());
            {
                if (ImGui::Checkbox("Attach Object...", &m_AttachObject))
                {
                    if (m_AttachObject) ExecCommand(COMMAND_CHANGE_ACTION, etaAdd);
                }
                ImGui::SameLine(0, 10);
                if (ImGui::Button("Detach Object", ImVec2(-1, 0)))
                {
                    ObjectList lst;
                    // every spawn point in the level, with no undo step written
                    if (mrYes == ELog.DlgMsg(mtConfirmation, mbYes | mbNo,
                            "Detach the object from ALL spawn points? This cannot be undone.") &&
                        Scene->GetQueryObjects(lst, OBJCLASS_SPAWNPOINT, 1, 1, 0)) {
                        for (ObjectIt it = lst.begin(); it != lst.end(); it++) {
                            CSpawnPoint* O = dynamic_cast<CSpawnPoint*>(*it); R_ASSERT(O);
                            O->DetachObject();
                        }
                    }
                }

            }
        }
        ImGui::Indent(ImGui::GetStyle().IndentSpacing);
        ImGui::TreePop();
	}
	// Only shape entities use it, but saying so costs a line and hunting for it costs
	// a restrictor that turns out to have no zone at all.
	ImGui::Separator();
	ImGui::TextDisabled("shape for restrictors and other zones");
	if (ImGui::RadioButton("Sphere##spawn_shape", m_ShapeSphere)) m_ShapeSphere = true;
	ImGui::SameLine();
	if (ImGui::RadioButton("Box##spawn_shape", !m_ShapeSphere)) m_ShapeSphere = false;

	ImGui::Separator();
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::TreeNode("Object List"))
	{
		ImGui::Unindent(ImGui::GetStyle().IndentSpacing);
		m_SpawnList->Draw();
		ImGui::Indent(ImGui::GetStyle().IndentSpacing);
        ImGui::TreePop();
    }
}

void UISpawnTool::SelByRefObject(bool flag)
{
    ObjectList objlist;
    LPCSTR N = Current();
    if (N) {
        ObjectIt _F = Scene->FirstObj(OBJCLASS_SPAWNPOINT);
        ObjectIt _E = Scene->LastObj(OBJCLASS_SPAWNPOINT);
        for (; _F != _E; _F++) {
            if ((*_F)->Visible()) {
                CSpawnPoint* _O = (CSpawnPoint*)(*_F);
                if (_O->RefCompare(N)) _O->Select(flag);
            }
        }
    }
}
void UISpawnTool::MultiSelByRefObject(bool clear_prev)
{
    ObjectList 	objlist;
    LPU32Vec 	sellist;
    if (Scene->GetQueryObjects(objlist, OBJCLASS_SPAWNPOINT, 1, 1, -1)) {
        for (ObjectIt it = objlist.begin(); it != objlist.end(); it++) {
            LPCSTR N = ((CSpawnPoint*)*it)->RefName();
            ObjectIt _F = Scene->FirstObj(OBJCLASS_SPAWNPOINT);
            ObjectIt _E = Scene->LastObj(OBJCLASS_SPAWNPOINT);
            for (; _F != _E; _F++) {
                CSpawnPoint* _O = (CSpawnPoint*)(*_F);
                if ((*_F)->Visible() && _O->RefCompare(N)) {
                    if (clear_prev) {
                        _O->Select(false);
                        sellist.push_back((u32*)_O);
                    }
                    else {
                        if (!_O->Selected())
                            sellist.push_back((u32*)_O);
                    }
                }
            }
        }
        std::sort(sellist.begin(), sellist.end());
        sellist.erase(std::unique(sellist.begin(), sellist.end()), sellist.end());
        std::shuffle(sellist.begin(), sellist.end(), xray::legacy_rand_urbg{});
        int max_k = iFloor(float(sellist.size()) / 100.f * float(m_selPercent) + 0.5f);
        int k = 0;
        for (LPU32It o_it = sellist.begin(); k < max_k; o_it++, k++) {
            CSpawnPoint* _O = (CSpawnPoint*)(*o_it);
            _O->Select(true);
        }
    }
}

// What a spawn class actually is. The list shows the leaf of the $spawn path, so a
// row reads "spawn group" or "graph point" and tells the author nothing about whether
// it is a zone, a marker, or engine bookkeeping they should not be placing at all.
static LPCSTR SpawnClassMeaning(LPCSTR cls)
{
	if (!cls) return 0;
	if (0 == _stricmp(cls, "SMRTTRRN"))	return "Smart terrain: owns NPCs and hands them jobs. Place one to get a smart of your own instead of hunting numbered ones";
	if (0 == _stricmp(cls, "SPC_RS_S"))	return "Zone with a shape. Quests use it as a place, a marker target, or a pen NPCs cannot leave";
	if (0 == _stricmp(cls, "SCRIPTZN"))	return "Script zone: a shape the level's own logic reacts to";
	if (0 == _stricmp(cls, "LVL_CHNG"))	return "Level changer: walking into it sends the player to another level";
	if (0 == _stricmp(cls, "AI_SPGRP"))	return "Spawn group (legacy): a respawn point of the old kind, not a smart terrain";
	if (0 == _stricmp(cls, "AI_GRAPH"))	return "Graph point: a vertex of the game graph, used when the level is compiled - not a runtime object";
	if (0 == _stricmp(cls, "SO_HLAMP"))	return "Hanging lamp";
	return 0;
}

void UISpawnTool::RefreshList()
{
    ListItemsVec items;
    LHelper().CreateItem(items, RPOINT_CHOOSE_NAME, 0, 0, 0);
    LHelper().CreateItem(items, ENVMOD_CHOOSE_NAME, 0, 0, 0);
    CInifile::Root& data = ((CInifile*)pSettings)->sections();
    for (CInifile::RootIt it = data.begin(); it != data.end(); it++) {
        LPCSTR val;
        if ((*it)->line_exist("$spawn", &val))
        {
            shared_str caption = pSettings->r_string_wb((*it)->Name, "$spawn");
            shared_str sect = (*it)->Name;
            if (caption.size())
            {
                ListItem* I = LHelper().CreateItem(items, caption.c_str(), 0, ListItem::flDrawThumbnail, (LPVOID) * (*it)->Name);
                if (I)
                {
                    // the section is the name everything else refers to, and the class
                    // is what decides whether this row is any use to a quest
                    LPCSTR cls = 0;
                    (*it)->line_exist("class", &cls);
                    string512 tip;
                    LPCSTR meaning = SpawnClassMeaning(cls);
                    sprintf_s(tip, "%s%s%s%s%s", sect.c_str(),
                        (cls && cls[0]) ? "  (class " : "", (cls && cls[0]) ? cls : "", (cls && cls[0]) ? ")" : "",
                        meaning ? "" : "");
                    if (meaning)
                    {
                        xr_strcat(tip, sizeof(tip), "\n");
                        xr_strcat(tip, sizeof(tip), meaning);
                    }
                    I->hint = tip;
                }
            }
        }
    }
    m_SpawnList->AssignItems(items);
}

void UISpawnTool::OnItemFocused(ListItem* item)
{
    m_Current = 0;
    if (item)
    {
        m_Current = (LPCSTR)item->m_Object;
    }
    ExecCommand(COMMAND_RENDER_FOCUS);
}
