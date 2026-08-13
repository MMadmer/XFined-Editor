#include "stdafx.h"

UIWorldOutliner* UIWorldOutliner::Form = nullptr;

// width of the per-row visibility toggle
static const float kEyeWidth = 22.f;

UIWorldOutliner::UIWorldOutliner()
{
	m_Signature				= 0;
	m_TargetClass			= OBJCLASS_DUMMY;
	m_TotalObjects			= 0;
	m_Dirty					= true;
	m_SelectedOnly			= false;
	m_SelectedOnlyApplied	= false;
	m_AnchorClass			= OBJCLASS_DUMMY;
	m_AnchorRow				= -1;
	m_SelSignature			= 0;
	m_ScrollTarget			= nullptr;
	m_SkipNextScroll		= false;
	m_PendingDelete			= nullptr;
	m_PendingRename			= nullptr;
	m_OpenRenamePopup		= false;
	m_Filter[0]				= 0;
	m_RenameBuf[0]			= 0;
	m_RenameError[0]		= 0;
}

UIWorldOutliner::~UIWorldOutliner()
{
}

void UIWorldOutliner::Show()
{
	if (!Form) Form = xr_new<UIWorldOutliner>();
	Form->bOpen		= true;
	Form->m_Dirty	= true;
}

void UIWorldOutliner::Close()
{
	xr_delete(Form);
}

void UIWorldOutliner::Update()
{
	if (!Form) return;
	Form->Draw();
	// runs even when the window is collapsed, so nothing stays queued forever
	Form->RunPending();
	if (Form->IsClosed()) Close();
}

void UIWorldOutliner::Refresh()
{
	if (Form) Form->m_Dirty = true;
}

//------------------------------------------------------------------------------
// cache
//------------------------------------------------------------------------------
// Per-class object counts folded into one value. std::list::size() is O(1), so
// this is a handful of integer ops per frame instead of a scene walk.
u32 UIWorldOutliner::SceneSignature()
{
	u32 sig = 2166136261u;
	for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
	{
		// ListObj VERIFYs on tools that hold no objects (ai_map and friends)
		if (!it->second || it->first == OBJCLASS_DUMMY || !Scene->GetOTool(it->first)) continue;
		sig = sig * 16777619u + u32(it->first);
		sig = sig * 16777619u + u32(Scene->ListObj(it->first).size());
	}
	return sig;
}

void UIWorldOutliner::Rebuild()
{
	// collect the qualifying tools first, so the groups can be sorted and then
	// filled in place - no vector-of-vectors copying
	xr_vector<ESceneToolBase*> tools;
	for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
	{
		if (!it->second || it->first == OBJCLASS_DUMMY || !Scene->GetOTool(it->first)) continue;
		if (Scene->ListObj(it->first).empty()) continue;
		tools.push_back(it->second);
	}

	std::sort(tools.begin(), tools.end(), [](ESceneToolBase* a, ESceneToolBase* b)
	{
		return xr_strcmp(a->ClassName(), b->ClassName()) < 0;
	});

	m_Groups.clear();
	m_Groups.resize(tools.size());
	m_TotalObjects = 0;

	for (u32 i = 0; i < tools.size(); ++i)
	{
		SGroup& g	= m_Groups[i];
		g.cls		= tools[i]->FClassID;
		g.name		= tools[i]->ClassName();
		g.objects.clear();
		ObjectList& lst = Scene->ListObj(g.cls);
		g.objects.reserve(lst.size());
		for (ObjectIt o = lst.begin(); o != lst.end(); ++o)
			g.objects.push_back(*o);
		m_TotalObjects += int(g.objects.size());
	}

	m_Signature		= SceneSignature();
	m_TargetClass	= LTools->CurrentClassID();
	m_Dirty			= false;
	// the anchor survives a rebuild on purpose - a plain click switches the
	// active target, which rebuilds right away; DrawRow range-checks it
	ApplyFilter		();
}

void UIWorldOutliner::ApplyFilter()
{
	char pat[sizeof(m_Filter)];
	strncpy_s(pat, sizeof(pat), m_Filter, _TRUNCATE);
	strlwr(pat);
	const bool has_pat = !!pat[0];

	for (u32 i = 0; i < m_Groups.size(); ++i)
	{
		SGroup& g = m_Groups[i];
		g.shown.clear();	// keeps the capacity, so steady state allocates nothing
		for (int k = 0; k < int(g.objects.size()); ++k)
		{
			CCustomObject* o = g.objects[k];
			if (m_SelectedOnly && !o->Selected()) continue;
			if (has_pat)
			{
				// SetName lowercases, but a hand-edited name may not be
				char low[256];
				strncpy_s(low, sizeof(low), o->GetName(), _TRUNCATE);
				strlwr(low);
				if (!strstr(low, pat)) continue;
			}
			g.shown.push_back(k);
		}
	}

	m_FilterApplied			= m_Filter;
	m_SelectedOnlyApplied	= m_SelectedOnly;
}

int UIWorldOutliner::ScanSelection(u32& sig, CCustomObject*& last) const
{
	// selected pointers folded in scene order: any change of the selected set
	// - grow, shrink or swap - lands on a different value
	int n	= 0;
	sig		= 2166136261u;
	last	= nullptr;
	for (u32 i = 0; i < m_Groups.size(); ++i)
	{
		const SGroup& g = m_Groups[i];
		for (u32 k = 0; k < g.objects.size(); ++k)
		{
			CCustomObject* o = g.objects[k];
			if (!o->Selected()) continue;
			++n;
			last = o;
			sig = sig * 16777619u + u32(size_t(o));
			sig = sig * 16777619u + u32(size_t(o) >> 32);
		}
	}
	return n;
}

//------------------------------------------------------------------------------
// selection / focus
//------------------------------------------------------------------------------
void UIWorldOutliner::PickObject(CCustomObject* obj)
{
	Scene->SelectObjects(false, OBJCLASS_DUMMY);
	obj->Select(TRUE);
	if (LTools->CurrentClassID() != obj->FClassID)
		LTools->SetTarget(obj->FClassID, 0);
	// the row is already under the cursor - recentring it would yank the list
	if (Form) Form->m_SkipNextScroll = true;
	UI->RedrawScene(true);
}

void UIWorldOutliner::FocusObject(CCustomObject* obj)
{
	Fbox box;
	if (!obj->GetBox(box))
	{
		const Fvector& p = obj->GetPosition();
		box.set(p, p);
		box.grow(2.f);
	}
	EDevice->m_Camera.ZoomExtents(box);
	UI->RedrawScene(true);
}

//------------------------------------------------------------------------------
// deferred scene edits
//------------------------------------------------------------------------------
void UIWorldOutliner::RunPending()
{
	if (!Scene || !m_PendingDelete) return;

	CCustomObject* obj	= m_PendingDelete;
	m_PendingDelete		= nullptr;

	if (Scene->locked())
	{
		ELog.DlgMsg(mtError, "Scene sharing violation");
		return;
	}

	Scene->SelectObjects(false, OBJCLASS_DUMMY);
	obj->Select(TRUE);
	const ObjClassID cls = obj->FClassID;
	if (LTools->CurrentClassID() != cls)
		LTools->SetTarget(cls, 0);

	// SetTarget is deferred to the next frame, so CurrentClassID() - the filter
	// COMMAND_DELETE_SELECTION uses - may still be the old class. Take the
	// command when it already matches, otherwise run its exact body.
	if (LTools->CurrentClassID() == cls)
	{
		ExecCommand(COMMAND_DELETE_SELECTION);
	}
	else
	{
		Scene->RemoveSelection(cls);
		Scene->UndoSave();
	}

	m_Dirty = true;
	UI->RedrawScene(true);
}

bool UIWorldOutliner::ApplyRename()
{
	m_RenameError[0] = 0;
	if (!m_PendingRename) return true;

	char low[sizeof(m_RenameBuf)];
	strncpy_s(low, sizeof(low), m_RenameBuf, _TRUNCATE);
	strlwr(low);
	if (!low[0])
	{
		strcpy_s(m_RenameError, "name can not be empty");
		return false;
	}
	// the pass_object overload ignores the object being renamed
	if (Scene->FindObjectByName(low, m_PendingRename))
	{
		strcpy_s(m_RenameError, "name already used");
		return false;
	}

	m_PendingRename->SetName(low);
	Scene->UndoSave();
	ExecCommand(COMMAND_UPDATE_PROPERTIES);
	m_Dirty = true;
	return true;
}

//------------------------------------------------------------------------------
// ui
//------------------------------------------------------------------------------
void UIWorldOutliner::DrawRow(SGroup& g, int row, CCustomObject* obj)
{
	ImGui::PushID(row);

	// visibility toggle; the ##id keeps the widget id stable across the label flip
	const bool vis = !!obj->Visible();
	if (ImGui::Button(vis ? "O##vis" : "-##vis", ImVec2(kEyeWidth, 0.f)))
	{
		obj->Show(vis ? FALSE : TRUE);
		UI->RedrawScene(true);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(vis ? "Visible" : "Hidden");
	ImGui::SameLine();

	// selection is read from the scene every frame, so viewport picks show here
	const bool sel = !!obj->Selected();
	const bool scroll_here = (obj == m_ScrollTarget);
	if (ImGui::Selectable(obj->GetName(), sel, ImGuiSelectableFlags_AllowDoubleClick,
		ImVec2(0.f, ImGui::GetFrameHeight())))
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			PickObject(obj);
			FocusObject(obj);
			m_AnchorClass = g.cls; m_AnchorRow = row;
		}
		else if (io.KeyShift && m_AnchorClass == g.cls &&
			m_AnchorRow >= 0 && m_AnchorRow < int(g.shown.size()))
		{
			const int a = _min(m_AnchorRow, row);
			const int b = _max(m_AnchorRow, row);
			for (int i = a; i <= b && i < int(g.shown.size()); ++i)
				g.objects[g.shown[i]]->Select(TRUE);
			m_SkipNextScroll = true;
			UI->RedrawScene(true);
		}
		else if (io.KeyCtrl)
		{
			obj->Select(sel ? FALSE : TRUE);
			m_AnchorClass = g.cls; m_AnchorRow = row;
			m_SkipNextScroll = true;
			UI->RedrawScene(true);
		}
		else
		{
			PickObject(obj);
			m_AnchorClass = g.cls; m_AnchorRow = row;
		}
	}
	// after the Selectable, so the scroll centres on the full row rect
	if (scroll_here)
	{
		ImGui::SetScrollHereY(0.5f);
		m_ScrollTarget = nullptr;
	}

	if (ImGui::BeginPopupContextItem("ctx"))
	{
		const bool cur_vis = !!obj->Visible();
		if (ImGui::MenuItem("Focus"))
		{
			PickObject(obj);
			FocusObject(obj);
		}
		if (ImGui::MenuItem("Select All Of This Class"))
		{
			Scene->SelectObjects(false, OBJCLASS_DUMMY);
			Scene->SelectObjects(true, g.cls);
			if (LTools->CurrentClassID() != g.cls)
				LTools->SetTarget(g.cls, 0);
			m_SkipNextScroll = true;
			UI->RedrawScene(true);
		}
		if (ImGui::MenuItem(cur_vis ? "Hide" : "Show"))
		{
			obj->Show(cur_vis ? FALSE : TRUE);
			UI->RedrawScene(true);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Rename..."))
		{
			m_PendingRename = obj;
			strncpy_s(m_RenameBuf, sizeof(m_RenameBuf), obj->GetName(), _TRUNCATE);
			m_RenameError[0]	= 0;
			m_OpenRenamePopup	= true;
		}
		if (ImGui::MenuItem("Delete"))
			m_PendingDelete = obj;
		ImGui::EndPopup();
	}

	ImGui::PopID();
}

void UIWorldOutliner::DrawGroup(SGroup& g)
{
	const bool filtering = Filtering();
	if (filtering && g.shown.empty()) return;	// empty groups vanish while filtering

	// the scroll target forces its group open and its row through the clipper -
	// otherwise a collapsed node or the clipped-out range would swallow the jump
	int target_row = -1;
	if (m_ScrollTarget)
		for (int i = 0; i < int(g.shown.size()); ++i)
			if (g.objects[g.shown[i]] == m_ScrollTarget) { target_row = i; break; }

	ImGui::PushID(int(g.cls));
	if (filtering || target_row >= 0) ImGui::SetNextItemOpen(true, ImGuiCond_Always);

	int flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
	const bool open = filtering
		? ImGui::TreeNodeEx("##group", flags, "%s (%d/%d)", g.name.c_str(), int(g.shown.size()), int(g.objects.size()))
		: ImGui::TreeNodeEx("##group", flags, "%s (%d)", g.name.c_str(), int(g.objects.size()));

	if (open)
	{
		// thousands of rows per group - only the visible slice is submitted
		ImGuiListClipper clipper;
		clipper.Begin(int(g.shown.size()), ImGui::GetFrameHeightWithSpacing());
		if (target_row >= 0)
			clipper.ForceDisplayRangeByIndices(target_row, target_row + 1);
		while (clipper.Step())
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
				DrawRow(g, row, g.objects[g.shown[row]]);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void UIWorldOutliner::DrawRenamePopup()
{
	if (!ImGui::BeginPopupModal("Rename Object", 0, ImGuiWindowFlags_AlwaysAutoResize)) return;

	ImGui::SetNextItemWidth(280.f);
	const bool enter = ImGui::InputText("##newname", m_RenameBuf, sizeof(m_RenameBuf),
		ImGuiInputTextFlags_EnterReturnsTrue);
	if (m_RenameError[0])
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", m_RenameError);

	const bool ok		= ImGui::Button("OK", ImVec2(90.f, 0.f)) || enter;
	ImGui::SameLine();
	const bool cancel	= ImGui::Button("Cancel", ImVec2(90.f, 0.f));

	if (ok)
	{
		if (ApplyRename())
		{
			m_PendingRename = nullptr;
			ImGui::CloseCurrentPopup();
		}
	}
	else if (cancel)
	{
		m_PendingRename = nullptr;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void UIWorldOutliner::Draw()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(280, 240));
	// a fresh panel opens usable instead of a 200px stub; the user's own size
	// wins afterwards because ImGui remembers it in imgui.ini
	ImGui::SetNextWindowSize(ImVec2(360, 560), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("World Outliner", &bOpen))
	{
		ImGui::PopStyleVar(1);
		ImGui::End();
		return;
	}

	if (!Scene)
	{
		ImGui::TextDisabled("no scene");
		ImGui::PopStyleVar(1);
		ImGui::End();
		return;
	}

	// cache maintenance: an explicit dirty flag, a per-class count probe and the
	// active tool - anything else (renames aside) cannot change the tree
	if (m_Dirty || m_TargetClass != LTools->CurrentClassID() || m_Signature != SceneSignature())
		Rebuild();

	// the selected-only view depends on live selection, so it is redone each
	// frame; a plain text filter only when the text itself changed
	if (m_SelectedOnly || m_SelectedOnlyApplied != m_SelectedOnly ||
		0 != xr_strcmp(m_FilterApplied.c_str(), m_Filter))
		ApplyFilter();

	// selection changes made OUTSIDE this panel (viewport picks, MCP) scroll
	// the last selected row into view; the panel's own clicks flag
	// m_SkipNextScroll, since their row is already visible
	u32 sel_sig;
	CCustomObject* sel_last;
	const int sel_count = ScanSelection(sel_sig, sel_last);
	if (sel_sig != m_SelSignature)
	{
		m_SelSignature = sel_sig;
		if (!m_SkipNextScroll && sel_last)
			m_ScrollTarget = sel_last;
		m_SkipNextScroll = false;
	}

	ImGui::SetNextItemWidth(-220.f);
	ImGui::InputTextWithHint("##filter", "search", m_Filter, sizeof(m_Filter));
	ImGui::SameLine();
	ImGui::Checkbox("Selected only", &m_SelectedOnly);
	ImGui::SameLine();
	if (ImGui::Button("Refresh")) m_Dirty = true;

	ImGui::Separator();

	if (ImGui::BeginChild("tree", ImVec2(0.f, -ImGui::GetFrameHeightWithSpacing()), false))
	{
		if (m_Groups.empty())
			ImGui::TextDisabled("scene is empty");
		for (u32 i = 0; i < m_Groups.size(); ++i)
			DrawGroup(m_Groups[i]);
	}
	ImGui::EndChild();
	// a target the filter hid never reaches DrawRow - one frame is its lifetime
	m_ScrollTarget = nullptr;

	ImGui::Separator();
	ImGui::Text("%d objects, %d selected", m_TotalObjects, sel_count);

	if (m_OpenRenamePopup)
	{
		ImGui::OpenPopup("Rename Object");
		m_OpenRenamePopup = false;
	}
	DrawRenamePopup();

	ImGui::PopStyleVar(1);
	ImGui::End();
}
