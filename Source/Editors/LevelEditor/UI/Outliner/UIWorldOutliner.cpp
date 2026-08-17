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
	m_Visibility				= 0;
	m_VisibilityApplied		= 0;
	m_Sort						= 0;
	m_SortApplied				= 0;
	m_AnchorClass			= OBJCLASS_DUMMY;
	m_SelSignature			= 0;
	m_ScrollTarget			= nullptr;
	m_SkipNextScroll		= false;
	m_OpenDeletePopup		= false;
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

// Unreal's search box splits on spaces and ANDs the results, treats a leading
// '-' as "must not contain", and keeps "quoted words" together. Cheap to do and
// it is what anyone who has used the engine's outliner expects to work.
void UIWorldOutliner::ParseFilter()
{
	m_Terms.clear();

	char low[sizeof(m_Filter)];
	strncpy_s(low, sizeof(low), m_Filter, _TRUNCATE);
	strlwr(low);

	for (const char* p = low; *p; )
	{
		while (*p == ' ' || *p == '\t') ++p;
		if (!*p) break;

		STerm t;
		t.exclude = false;
		// a bare '-' is someone mid-typing, not an empty exclusion
		if (*p == '-' && p[1] && p[1] != ' ') { t.exclude = true; ++p; }

		if (*p == '"')
		{
			++p;
			const char* e = strchr(p, '"');
			if (!e) e = p + xr_strlen(p);		// unclosed quote: take the rest
			t.text.assign(p, e);
			p = *e ? e + 1 : e;
		}
		else
		{
			const char* e = p;
			while (*e && *e != ' ' && *e != '\t') ++e;
			t.text.assign(p, e);
			p = e;
		}

		if (!t.text.empty()) m_Terms.push_back(t);
	}
}

bool UIWorldOutliner::NameMatches(LPCSTR name) const
{
	if (m_Terms.empty()) return true;

	// SetName lowercases, but a hand-edited name may not be
	char low[256];
	strncpy_s(low, sizeof(low), name, _TRUNCATE);
	strlwr(low);

	for (u32 i = 0; i < m_Terms.size(); ++i)
	{
		// a plain term that misses, or an excluded term that hits, both reject
		const bool hit = !!strstr(low, m_Terms[i].text.c_str());
		if (hit == m_Terms[i].exclude) return false;
	}
	return true;
}

bool UIWorldOutliner::ClassHidden(ObjClassID cls) const
{
	for (u32 i = 0; i < m_HiddenClasses.size(); ++i)
		if (m_HiddenClasses[i] == cls) return true;
	return false;
}

int UIWorldOutliner::NaturalCompare(LPCSTR lhs, LPCSTR rhs)
{
	while (*lhs && *rhs)
	{
		if (isdigit(static_cast<unsigned char>(*lhs)) && isdigit(static_cast<unsigned char>(*rhs)))
		{
			const char* lhs_begin = lhs;
			const char* rhs_begin = rhs;
			while (*lhs == '0') ++lhs;
			while (*rhs == '0') ++rhs;

			const char* lhs_end = lhs;
			const char* rhs_end = rhs;
			while (isdigit(static_cast<unsigned char>(*lhs_end))) ++lhs_end;
			while (isdigit(static_cast<unsigned char>(*rhs_end))) ++rhs_end;

			const size_t lhs_digits = lhs_end - lhs;
			const size_t rhs_digits = rhs_end - rhs;
			if (lhs_digits != rhs_digits) return lhs_digits < rhs_digits ? -1 : 1;
			const int number_cmp = strncmp(lhs, rhs, lhs_digits);
			if (number_cmp) return number_cmp;

			const size_t lhs_width = lhs_end - lhs_begin;
			const size_t rhs_width = rhs_end - rhs_begin;
			if (lhs_width != rhs_width) return lhs_width < rhs_width ? -1 : 1;
			lhs = lhs_end;
			rhs = rhs_end;
			continue;
		}

		const int lhs_char = tolower(static_cast<unsigned char>(*lhs));
		const int rhs_char = tolower(static_cast<unsigned char>(*rhs));
		if (lhs_char != rhs_char) return lhs_char < rhs_char ? -1 : 1;
		++lhs;
		++rhs;
	}
	return *lhs ? 1 : (*rhs ? -1 : 0);
}

void UIWorldOutliner::ApplyFilter()
{
	for (u32 i = 0; i < m_Groups.size(); ++i)
	{
		SGroup& g = m_Groups[i];
		g.shown.clear();	// keeps the capacity, so steady state allocates nothing
		for (int k = 0; k < int(g.objects.size()); ++k)
		{
			CCustomObject* o = g.objects[k];
			if (m_SelectedOnly && !o->Selected())	continue;
			if (1 == m_Visibility && !o->Visible())	continue;
			if (2 == m_Visibility && o->Visible())	continue;
			if (!NameMatches(o->GetName()))			continue;
			g.shown.push_back(k);
		}

		if (m_Sort)
		{
			std::stable_sort(g.shown.begin(), g.shown.end(), [this, &g](int lhs, int rhs)
			{
				const int cmp = NaturalCompare(g.objects[lhs]->GetName(), g.objects[rhs]->GetName());
				return 1 == m_Sort ? cmp < 0 : cmp > 0;
			});
		}
	}

	m_FilterApplied			= m_Filter;
	m_SelectedOnlyApplied	= m_SelectedOnly;
	m_VisibilityApplied		= m_Visibility;
	m_SortApplied				= m_Sort;
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
// MCP surface
//------------------------------------------------------------------------------
bool UIWorldOutliner::McpSetFilter(LPCSTR text, int selected_only, LPCSTR types,
	LPCSTR visibility, xr_string& err)
{
	Show();									// filtering a closed panel helps nobody
	if (!Form)	{ err = "outliner unavailable"; return false; }

	int next_visibility = Form->m_Visibility;
	if (visibility)
	{
		if (0 == _stricmp(visibility, "all"))			next_visibility = 0;
		else if (0 == _stricmp(visibility, "visible"))	next_visibility = 1;
		else if (0 == _stricmp(visibility, "hidden"))	next_visibility = 2;
		else
		{
			err = "visibility must be all, visible, or hidden";
			return false;
		}
	}

	xr_vector<ObjClassID> hide;
	if (types)
	{
		// class names are matched against the cache, which may not exist yet
		if (Form->m_Groups.empty()) Form->Rebuild();

		if (types[0])
		{
			xr_string unknown;
			// every listed name has to name a group, or the caller silently
			// gets an empty tree and no idea why
			for (const char* p = types; *p; )
			{
				const char* e = strchr(p, ';');
				if (!e) e = p + xr_strlen(p);
				xr_string one; one.assign(p, e);
				while (!one.empty() && one[0] == ' ')					one.erase(0, 1);
				while (!one.empty() && one[one.size()-1] == ' ')			one.erase(one.size()-1);
				if (!one.empty())
				{
					bool known = false;
					for (u32 i = 0; i < Form->m_Groups.size() && !known; ++i)
						known = (0 == _stricmp(Form->m_Groups[i].name.c_str(), one.c_str()));
					if (!known) { if (!unknown.empty()) unknown += ", "; unknown += one; }
				}
				p = *e ? e + 1 : e;
			}
			if (!unknown.empty())
			{
				err  = "unknown object type(s): " + unknown + "; scene has: ";
				for (u32 i = 0; i < Form->m_Groups.size(); ++i)
				{
					if (i) err += ", ";
					err += Form->m_Groups[i].name;
				}
				return false;
			}

			for (u32 i = 0; i < Form->m_Groups.size(); ++i)
			{
				bool shown = false;
				for (const char* p = types; *p && !shown; )
				{
					const char* e = strchr(p, ';');
					if (!e) e = p + xr_strlen(p);
					xr_string one; one.assign(p, e);
					while (!one.empty() && one[0] == ' ')				one.erase(0, 1);
					while (!one.empty() && one[one.size()-1] == ' ')		one.erase(one.size()-1);
					shown = (0 == _stricmp(Form->m_Groups[i].name.c_str(), one.c_str()));
					p = *e ? e + 1 : e;
				}
				if (!shown) hide.push_back(Form->m_Groups[i].cls);
			}
		}
	}

	// A rejected request must not leave half of its filter applied.
	if (text)					strncpy_s(Form->m_Filter, sizeof(Form->m_Filter), text, _TRUNCATE);
	if (selected_only >= 0)		Form->m_SelectedOnly = !!selected_only;
	if (types)					Form->m_HiddenClasses = hide;
	Form->m_Visibility = next_visibility;
	Form->ParseFilter();
	Form->ApplyFilter();
	return true;
}

void UIWorldOutliner::McpGetFilter(xr_string& text, bool& selected_only,
								   xr_string& hidden, xr_string& visibility,
								   int& shown, int& total)
{
	text.clear(); hidden.clear(); visibility = "all";
	selected_only	= false;
	shown = total	= 0;
	if (!Form) return;

	text			= Form->m_Filter;
	selected_only	= Form->m_SelectedOnly;
	visibility		= 1 == Form->m_Visibility ? "visible" : (2 == Form->m_Visibility ? "hidden" : "all");
	total			= Form->m_TotalObjects;
	for (u32 i = 0; i < Form->m_Groups.size(); ++i)
	{
		if (Form->ClassHidden(Form->m_Groups[i].cls))
		{
			if (!hidden.empty()) hidden += ";";
			hidden += Form->m_Groups[i].name;
			continue;
		}
		shown += int(Form->m_Groups[i].shown.size());
	}
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
	if (!Scene || !m_PendingDeleteName.size()) return;

	const shared_str name = m_PendingDeleteName;
	m_PendingDeleteName = "";
	CCustomObject* obj = Scene->FindObjectByName(name.c_str(), static_cast<CCustomObject*>(nullptr));
	if (!obj) return;

	if (Scene->locked() || !obj->FParentTools || !obj->FParentTools->IsEditable() || !obj->Editable())
	{
		ELog.DlgMsg(mtError, "Object '%s' is read only", obj->GetName());
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
	if (!m_RenameOriginal.size()) return true;
	CCustomObject* obj = Scene
		? Scene->FindObjectByName(m_RenameOriginal.c_str(), static_cast<CCustomObject*>(nullptr)) : nullptr;
	if (!obj)
	{
		strcpy_s(m_RenameError, "object no longer exists");
		return false;
	}
	if (Scene->locked() || !obj->FParentTools || !obj->FParentTools->IsEditable() || !obj->Editable())
	{
		strcpy_s(m_RenameError, "object is read only");
		return false;
	}

	char low[sizeof(m_RenameBuf)];
	strncpy_s(low, sizeof(low), m_RenameBuf, _TRUNCATE);
	strlwr(low);
	if (!low[0])
	{
		strcpy_s(m_RenameError, "name can not be empty");
		return false;
	}
	// the pass_object overload ignores the object being renamed
	if (Scene->FindObjectByName(low, obj))
	{
		strcpy_s(m_RenameError, "name already used");
		return false;
	}

	if (m_AnchorName.equal(m_RenameOriginal)) m_AnchorName = low;
	obj->SetName(low);
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
	ImGui::TableNextRow();
	ImGui::PushID(row);

	// visibility toggle; the ##id keeps the widget id stable across the label flip
	ImGui::TableSetColumnIndex(0);
	const bool vis = !!obj->Visible();
	if (ImGui::Button(vis ? "O##vis" : "-##vis", ImVec2(kEyeWidth, 0.f)))
	{
		obj->Show(vis ? FALSE : TRUE);
		UI->RedrawScene(true);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(vis ? "Visible" : "Hidden");

	// selection is read from the scene every frame, so viewport picks show here
	ImGui::TableSetColumnIndex(1);
	const bool sel = !!obj->Selected();
	if (ImGui::Selectable(obj->GetName(), sel,
		ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns,
		ImVec2(0.f, ImGui::GetFrameHeight())))
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			PickObject(obj);
			FocusObject(obj);
			m_AnchorClass = g.cls; m_AnchorName = obj->GetName();
		}
		else if (io.KeyShift && m_AnchorClass == g.cls && m_AnchorName.size())
		{
			int anchor = -1;
			for (int i = 0; i < int(g.shown.size()); ++i)
				if (m_AnchorName.equal(g.objects[g.shown[i]]->GetName())) { anchor = i; break; }
			if (anchor >= 0)
			{
				const int a = _min(anchor, row);
				const int b = _max(anchor, row);
				for (int i = a; i <= b; ++i)
					g.objects[g.shown[i]]->Select(TRUE);
				m_SkipNextScroll = true;
				UI->RedrawScene(true);
			}
			else
			{
				PickObject(obj);
				m_AnchorName = obj->GetName();
			}
		}
		else if (io.KeyCtrl)
		{
			obj->Select(sel ? FALSE : TRUE);
			m_AnchorClass = g.cls; m_AnchorName = obj->GetName();
			m_SkipNextScroll = true;
			UI->RedrawScene(true);
		}
		else
		{
			PickObject(obj);
			m_AnchorClass = g.cls; m_AnchorName = obj->GetName();
		}
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
		const bool can_edit = !Scene->locked() && obj->Editable() && obj->FParentTools &&
			!!obj->FParentTools->IsEditable();
		ImGui::BeginDisabled(!can_edit);
		if (ImGui::MenuItem("Rename..."))
		{
			m_RenameOriginal = obj->GetName();
			strncpy_s(m_RenameBuf, sizeof(m_RenameBuf), obj->GetName(), _TRUNCATE);
			m_RenameError[0]	= 0;
			m_OpenRenamePopup	= true;
		}
		if (ImGui::MenuItem("Delete"))
		{
			m_DeleteName = obj->GetName();
			m_OpenDeletePopup = true;
		}
		ImGui::EndDisabled();
		ImGui::EndPopup();
	}

	ImGui::TableSetColumnIndex(2);
	ImGui::TextUnformatted(g.name.c_str());
	ImGui::TableSetColumnIndex(3);
	CCustomObject* owner = obj->GetOwner();
	if (owner) ImGui::TextUnformatted(owner->GetName());

	ImGui::PopID();
}

void UIWorldOutliner::ScrollRowIntoView(float row_top, float row_h)
{
	const float view_top	= ImGui::GetScrollY();
	const float view_h		= ImGui::GetWindowHeight();

	// Already fully on screen: leave it EXACTLY where it is. Unreal checks the
	// same thing before scrolling (SListView::ScrollIntoView, "Only scroll the
	// item into view if it's not already in the visible range") and it is the
	// difference between a list that helps and one that runs away from the
	// cursor every time something is picked.
	if (row_top >= view_top && row_top + row_h <= view_top + view_h) return;

	// Off screen: centre it, which is what STreeView does - its default
	// alignment is CenterAligned, offset = index - NumVisible/2. ImGui clamps
	// the target to the scroll range for us.
	ImGui::SetScrollY(row_top + row_h * 0.5f - view_h * 0.5f);
}

void UIWorldOutliner::DrawGroup(SGroup& g)
{
	if (ClassHidden(g.cls)) return;				// switched off in the funnel

	const bool filtering = Filtering();
	if (filtering && g.shown.empty()) return;	// empty groups vanish while filtering

	// a scroll target in a collapsed group would never come into view
	int target_row = -1;
	if (m_ScrollTarget)
		for (int i = 0; i < int(g.shown.size()); ++i)
			if (g.objects[g.shown[i]] == m_ScrollTarget) { target_row = i; break; }

	ImGui::PushID(int(g.cls));
	if (filtering || target_row >= 0) ImGui::SetNextItemOpen(true, ImGuiCond_Always);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(1);
	int flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_SpanAllColumns |
		ImGuiTreeNodeFlags_DefaultOpen;
	const bool open = filtering
		? ImGui::TreeNodeEx("##group", flags, "%s (%d/%d)", g.name.c_str(), int(g.shown.size()), int(g.objects.size()))
		: ImGui::TreeNodeEx("##group", flags, "%s (%d)", g.name.c_str(), int(g.objects.size()));

	if (open)
	{
		// Rows are a fixed height, so the target's position is arithmetic - it
		// never has to be drawn to be scrolled to. GetCursorPosY is content
		// space, the same space GetScrollY/SetScrollY speak.
		const float rows_top	= ImGui::GetCursorPosY();
		const float row_h		= ImGui::GetFrameHeightWithSpacing();

		// thousands of rows per group - only the visible slice is submitted
		ImGuiListClipper clipper;
		clipper.Begin(int(g.shown.size()), row_h);
		while (clipper.Step())
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
				DrawRow(g, row, g.objects[g.shown[row]]);
		ImGui::TreePop();

		if (target_row >= 0) ScrollRowIntoView(rows_top + target_row * row_h, row_h);
	}
	ImGui::PopID();
}

// The funnel beside the search box, Unreal's "Filters" menu in miniature: the
// object types the tree is allowed to show. Types are hidden rather than shown
// so a scene growing a new one does not silently stay invisible.
void UIWorldOutliner::DrawFilterMenu()
{
	if (!ImGui::BeginPopup("outliner_filters")) return;

	ImGui::TextDisabled("Visibility");
	const char* visibility[] = { "All", "Visible", "Hidden" };
	ImGui::SetNextItemWidth(150.f);
	ImGui::Combo("##visibility", &m_Visibility, visibility, IM_ARRAYSIZE(visibility));
	ImGui::Separator();

	ImGui::TextDisabled("Object types");
	ImGui::Separator();

	if (ImGui::SmallButton("All"))
		m_HiddenClasses.clear();
	ImGui::SameLine();
	if (ImGui::SmallButton("None"))
	{
		m_HiddenClasses.clear();
		for (u32 i = 0; i < m_Groups.size(); ++i)
			m_HiddenClasses.push_back(m_Groups[i].cls);
	}
	ImGui::Separator();

	for (u32 i = 0; i < m_Groups.size(); ++i)
	{
		SGroup& g	= m_Groups[i];
		bool shown	= !ClassHidden(g.cls);
		ImGui::PushID(int(g.cls));
		if (ImGui::Checkbox(g.name.c_str(), &shown))
		{
			if (shown)
			{
				for (u32 k = 0; k < m_HiddenClasses.size(); ++k)
					if (m_HiddenClasses[k] == g.cls) { m_HiddenClasses.erase(m_HiddenClasses.begin() + k); break; }
			}
			else m_HiddenClasses.push_back(g.cls);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(%d)", int(g.objects.size()));
		ImGui::PopID();
	}

	ImGui::EndPopup();
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
			m_RenameOriginal = "";
			ImGui::CloseCurrentPopup();
		}
	}
	else if (cancel)
	{
		m_RenameOriginal = "";
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void UIWorldOutliner::DrawDeletePopup()
{
	if (!ImGui::BeginPopupModal("Delete Object", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

	ImGui::Text("Delete '%s'?", m_DeleteName.size() ? m_DeleteName.c_str() : "missing object");
	ImGui::TextDisabled("The operation can be undone from the editor.");
	ImGui::Separator();

	const bool cancel = ImGui::Button("Cancel", ImVec2(100.f, 0.f)) ||
		ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	ImGui::SetItemDefaultFocus();
	ImGui::SameLine();
	const bool remove = ImGui::Button("Delete", ImVec2(100.f, 0.f));

	if (cancel)
	{
		m_DeleteName = "";
		ImGui::CloseCurrentPopup();
	}
	else if (remove)
	{
		m_PendingDeleteName = m_DeleteName;
		m_DeleteName = "";
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
	const bool text_changed = (0 != xr_strcmp(m_FilterApplied.c_str(), m_Filter));
	if (text_changed) ParseFilter();
	if (m_SelectedOnly || m_Visibility || m_SelectedOnlyApplied != m_SelectedOnly ||
		m_VisibilityApplied != m_Visibility || m_SortApplied != m_Sort || text_changed)
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

	// funnel first, search box next: the Unreal arrangement
	if (ImGui::Button("Filters")) ImGui::OpenPopup("outliner_filters");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("which object types the tree shows%s",
			m_HiddenClasses.empty() ? "" : "  (some are hidden)");
	DrawFilterMenu();
	ImGui::SameLine();

	ImGui::SetNextItemWidth(-220.f);
	ImGui::InputTextWithHint("##filter", "search", m_Filter, sizeof(m_Filter));
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("every word has to match\n-word excludes it\n\"two words\" match together");
	ImGui::SameLine();
	ImGui::Checkbox("Selected only", &m_SelectedOnly);
	ImGui::SameLine();
	if (ImGui::Button("Refresh")) m_Dirty = true;

	ImGui::Separator();

	const ImGuiTableFlags table_flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_RowBg |
		ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
		ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate | ImGuiTableFlags_ScrollY;
	if (ImGui::BeginTable("tree", 4, table_flags,
		ImVec2(0.f, -ImGui::GetFrameHeightWithSpacing())))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed |
			ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHide, 48.f);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.6f, 1);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed |
			ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_DefaultHide, 90.f);
		ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch |
			ImGuiTableColumnFlags_NoSort, 0.4f);
		ImGui::TableHeadersRow();

		int sort = 0;
		const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
		if (specs && specs->SpecsCount)
		{
			const ImGuiSortDirection direction = specs->Specs[0].SortDirection;
			if (ImGuiSortDirection_Ascending == direction) sort = 1;
			else if (ImGuiSortDirection_Descending == direction) sort = 2;
		}
		if (sort != m_Sort)
		{
			m_Sort = sort;
			ApplyFilter();
		}

		if (m_Groups.empty())
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("scene is empty");
		}
		for (u32 i = 0; i < m_Groups.size(); ++i)
			DrawGroup(m_Groups[i]);
		ImGui::EndTable();
	}
	// a target the filter hid never reaches DrawRow - one frame is its lifetime
	m_ScrollTarget = nullptr;

	ImGui::Separator();
	// with a filter or a hidden type on, say how much of the scene is showing
	int shown = 0;
	for (u32 i = 0; i < m_Groups.size(); ++i)
		if (!ClassHidden(m_Groups[i].cls)) shown += int(m_Groups[i].shown.size());
	if (shown != m_TotalObjects)
		ImGui::Text("%d of %d objects, %d selected", shown, m_TotalObjects, sel_count);
	else
		ImGui::Text("%d objects, %d selected", m_TotalObjects, sel_count);

	if (m_OpenRenamePopup)
	{
		ImGui::OpenPopup("Rename Object");
		m_OpenRenamePopup = false;
	}
	DrawRenamePopup();
	if (m_OpenDeletePopup)
	{
		ImGui::OpenPopup("Delete Object");
		m_OpenDeletePopup = false;
	}
	DrawDeletePopup();

	ImGui::PopStyleVar(1);
	ImGui::End();
}
