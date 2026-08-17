#include "stdafx.h"
#include "UIQuestGraph.h"
#include "NqCanvas.h"
#include "NqInspector.h"
#include "../../../XrECore/Editor/Nq/NqUtil.h"
#include "../../../XrECore/Editor/Nq/NqCatalog.h"
#include "../../../XrECore/Editor/Nq/NqLayout.h"
#include "../../../XrECore/Editor/Nq/NqLua.h"

namespace
{
	xr_vector<UIQuestGraphWindow*> s_Windows;
	xr_string s_LastProject;

	// Canvas | inspector geometry in frame heights, not pixels: every style metric
	// is baked with the monitor DPI once at init (XrUIManager::Initialize), so raw
	// pixel minima would shrink to a sliver on a scaled display.
	const float	s_CanvasMinEm		= 11.f;		// ~200 px at 100%
	const float	s_InspectorMinEm	= 16.f;		// ~290 px at 100%
	const float	s_GrabEm			= 0.35f;	// drag handle, plus one ItemSpacing
	const u32	s_SplitDefault		= 320;		// permille of the body width

	struct SSearchTerm
	{
		xr_string	text;
		bool		exclude;
	};

	float SplitGrab()	{ return ImGui::GetFrameHeight() * s_GrabEm + ImGui::GetStyle().ItemSpacing.x; }

	// the split lives in the editor preferences beside the content browser's tree
	// width, so it survives a restart and not just a reopened tab
	float LoadSplit()
	{
		u32 permille = s_SplitDefault;
		// preferences may not exist yet when the first tab is constructed
		if (CLevelPreferences* prefs = dynamic_cast<CLevelPreferences*>(EPrefs)) permille = prefs->QuestInspectorSplit;
		return float(_max(_min(permille, 950u), 50u)) / 1000.f;
	}

	void SaveSplit(float frac)
	{
		if (CLevelPreferences* prefs = dynamic_cast<CLevelPreferences*>(EPrefs))
			prefs->QuestInspectorSplit = u32(_max(_min(frac, 0.95f), 0.05f) * 1000.f + 0.5f);
	}

	// Match the picker search for both ASCII and Cyrillic UTF-8 text.
	void FoldSearch(LPCSTR text, xr_string& out)
	{
		out.clear();
		if (!text) return;
		for (const u8* p = (const u8*)text; *p; ++p)
		{
			const u8 c = *p;
			if (c >= 'A' && c <= 'Z') { out += char(c + 0x20); continue; }
			if (c == 0xD0 && p[1])
			{
				const u8 d = p[1];
				if (d == 0x81)				{ out += char(0xD1); out += char(0x91); ++p; continue; }
				if (d >= 0x90 && d <= 0x9F)	{ out += char(0xD0); out += char(d + 0x20); ++p; continue; }
				if (d >= 0xA0 && d <= 0xAF)	{ out += char(0xD1); out += char(d - 0x20); ++p; continue; }
			}
			out += char(c);
		}
	}

	void ParseSearch(LPCSTR query, xr_vector<SSearchTerm>& out)
	{
		out.clear();
		const char* p = query ? query : "";
		while (*p)
		{
			while (*p && (u8)*p <= 0x20) ++p;
			if (!*p) break;
			SSearchTerm term;
			term.exclude = *p == '-' && p[1] && (u8)p[1] > 0x20;
			if (term.exclude) ++p;
			const bool quoted = *p == '"';
			if (quoted) ++p;
			const char* begin = p;
			if (quoted) while (*p && *p != '"') ++p;
			else while (*p && (u8)*p > 0x20) ++p;
			xr_string raw;
			raw.assign(begin, p - begin);
			if (quoted && *p == '"') ++p;
			FoldSearch(raw.c_str(), term.text);
			if (!term.text.empty()) out.push_back(term);
		}
	}

	bool SearchMatches(LPCSTR text, const xr_vector<SSearchTerm>& terms)
	{
		xr_string folded;
		FoldSearch(text, folded);
		for (u32 i = 0; i < terms.size(); ++i)
		{
			const bool found = strstr(folded.c_str(), terms[i].text.c_str());
			if ((terms[i].exclude && found) || (!terms[i].exclude && !found)) return false;
		}
		return !terms.empty();
	}

	bool SearchFieldHit(LPCSTR text, const xr_vector<SSearchTerm>& terms)
	{
		xr_string folded;
		FoldSearch(text, folded);
		for (u32 i = 0; i < terms.size(); ++i)
			if (!terms[i].exclude && strstr(folded.c_str(), terms[i].text.c_str())) return true;
		return false;
	}
}

//------------------------------------------------------------------------------
// window
//------------------------------------------------------------------------------
UIQuestGraphWindow::UIQuestGraphWindow(NqDoc* doc) : m_Doc(doc)
{
	m_Canvas = xr_new<NqCanvas>(doc);
	m_Inspector = xr_new<NqInspector>(doc);
	m_SplitFrac = LoadSplit();
	m_Focus = true;
	m_AskClose = m_CloseNow = false;
	m_ShowProblems = false;
	m_ShowFind = m_FocusFind = false;
	m_Find[0] = 0;
	m_FindIndex = -1;
	m_FindRevision = u32(-1);
	m_FindCatalogGeneration = u32(-1);
}

UIQuestGraphWindow::~UIQuestGraphWindow()
{
	xr_delete(m_Canvas);
	xr_delete(m_Inspector);
}

xr_string UIQuestGraphWindow::Title() const
{
	// the label carries the file name; the imgui id is pinned to the path
	xr_string name = NqUtil::BaseName(m_Doc->path.c_str());
	return NqUtil::Format("Quest: %s%s###nq_%s", name.c_str(), m_Doc->dirty ? " *" : "", NqUtil::PathKey(m_Doc->path.c_str()).c_str());
}

void UIQuestGraphWindow::RequestClose(bool discard)
{
	if (m_Doc->dirty && !discard) m_AskClose = true;
	else m_CloseNow = true;
}

void UIQuestGraphWindow::Save()
{
	xr_string err;
	if (m_Doc->Save(false, err)) m_Message = "saved";
	else if (err == "modified externally") m_Message = "file changed on disk - use Reload or Save (force)";
	else m_Message = "save failed: " + err;
}

void UIQuestGraphWindow::SetFindQuery(LPCSTR query)
{
	strncpy_s(m_Find, sizeof(m_Find), query ? query : "", _TRUNCATE);
	m_ShowFind = true;
	RefreshFind(true);
}

void UIQuestGraphWindow::RefreshFind(bool force)
{
	const u32 catalog_generation = NqCatalog::Generation();
	if (!force && m_FindRevision == m_Doc->revision && m_FindCatalogGeneration == catalog_generation) return;
	xr_string active;
	if (m_FindIndex >= 0 && m_FindIndex < (int)m_FindResults.size()) active = m_FindResults[m_FindIndex].node;
	m_FindResults.clear();
	m_FindIndex = -1;
	m_FindRevision = m_Doc->revision;
	m_FindCatalogGeneration = catalog_generation;

	xr_vector<SSearchTerm> terms;
	ParseSearch(NqUtil::Trim(m_Find).c_str(), terms);
	if (terms.empty()) return;
	for (u32 i = 0; i < m_Doc->quest.nodes.size(); ++i)
	{
		const SNqNode& node = m_Doc->quest.nodes[i];
		const NqCatalog::SKind* kind = NqCatalog::Find(node.kind.c_str());
		const xr_string title = kind ? kind->title : node.kind;
		xr_string searchable = NqLua::WriteNode(node, 0);
		searchable += "\n";
		searchable += title;
		if (!SearchMatches(searchable.c_str(), terms)) continue;

		SFindResult result;
		result.node = node.id;
		result.kind = node.kind;
		result.title = title;
		if (SearchFieldHit(node.id.c_str(), terms)) result.match = "id";
		else if (SearchFieldHit(node.kind.c_str(), terms) || SearchFieldHit(title.c_str(), terms)) result.match = "kind";
		else if (SearchFieldHit(node.comment.c_str(), terms)) result.match = "comment";
		else result.match = "content";
		m_FindResults.push_back(result);
		if (!active.empty() && active == node.id) m_FindIndex = (int)m_FindResults.size() - 1;
	}
}

bool UIQuestGraphWindow::NavigateFind(int index)
{
	RefreshFind();
	if (m_FindResults.empty()) { m_FindIndex = -1; return false; }
	if (index < 0) index = (int)m_FindResults.size() - 1;
	if (index >= (int)m_FindResults.size()) index = 0;
	m_FindIndex = index;
	m_Canvas->RequestFrameNode(m_FindResults[index].node.c_str());
	Focus();
	return true;
}

void UIQuestGraphWindow::AppendViewState(xr_string& out)
{
	out += NqUtil::Format(",\"zoom_level\":%d,\"zoom\":%.3f,\"center\":[%.1f,%.1f]",
		m_Doc->zoom_idx, m_Canvas->Zoom(), m_Doc->view_cx, m_Doc->view_cy);
	out += ",\"selected\":";
	NqUtil::JsonStringArray(out, m_Doc->selection);
	out += ",\"slot\":";
	NqUtil::JsonString(out, m_Doc->sel_slot);
}

void UIQuestGraphWindow::AppendFindState(xr_string& out, int limit)
{
	RefreshFind();
	limit = _max(1, _min(limit, 500));
	out += ",\"query\":";
	NqUtil::JsonString(out, m_Find);
	out += ",\"count\":";
	NqUtil::JsonInt(out, (int)m_FindResults.size());
	out += ",\"index\":";
	NqUtil::JsonInt(out, m_FindIndex);
	out += ",\"current\":";
	if (m_FindIndex >= 0 && m_FindIndex < (int)m_FindResults.size()) NqUtil::JsonString(out, m_FindResults[m_FindIndex].node);
	else out += "null";
	out += ",\"results\":[";
	const int count = _min(limit, (int)m_FindResults.size());
	for (int i = 0; i < count; ++i)
	{
		if (i) out += ",";
		out += "{\"node\":"; NqUtil::JsonString(out, m_FindResults[i].node);
		out += ",\"kind\":"; NqUtil::JsonString(out, m_FindResults[i].kind);
		out += ",\"title\":"; NqUtil::JsonString(out, m_FindResults[i].title);
		out += ",\"match\":"; NqUtil::JsonString(out, m_FindResults[i].match);
		out += "}";
	}
	out += "],\"truncated\":";
	NqUtil::JsonBool(out, count < (int)m_FindResults.size());
	AppendViewState(out);
}

void UIQuestGraphWindow::DrawFindBar()
{
	RefreshFind();
	const bool can_back = !m_Canvas->BackHistory().empty();
	const bool can_forward = !m_Canvas->ForwardHistory().empty();
	ImGui::BeginDisabled(!can_back);
	if (ImGui::SmallButton("<###nq_history_back")) m_Canvas->HistoryBack();
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back (Alt+Left)");
	ImGui::SameLine();
	ImGui::BeginDisabled(!can_forward);
	if (ImGui::SmallButton(">###nq_history_forward")) m_Canvas->HistoryForward();
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward (Alt+Right)");

	ImGui::SameLine();
	if (ImGui::SmallButton("Find###nq_find_toggle")) { m_ShowFind = true; m_FocusFind = true; }
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Find nodes (Ctrl+F, F3 next)");

	ImGui::SameLine();
	const xr_string bookmark_label = NqUtil::Format("Bookmarks (%d)", (int)m_Canvas->Bookmarks().size());
	if (ImGui::BeginCombo("##nq_bookmarks", bookmark_label.c_str()))
	{
		const xr_vector<xr_string>& bookmarks = m_Canvas->Bookmarks();
		for (u32 i = 0; i < bookmarks.size(); ++i)
			if (m_Doc->quest.FindNode(bookmarks[i].c_str()) && ImGui::Selectable(bookmarks[i].c_str())) m_Canvas->RequestFrameNode(bookmarks[i].c_str());
		if (bookmarks.empty()) ImGui::TextDisabled("No bookmarks (Ctrl+B)");
		if (!bookmarks.empty())
		{
			ImGui::Separator();
			if (ImGui::Selectable("Clear bookmarks")) m_Canvas->ClearBookmarks();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(m_Canvas->Bookmarks().empty());
	if (ImGui::SmallButton("B<###nq_bookmark_prev")) m_Canvas->JumpBookmark(-1);
	ImGui::SameLine();
	if (ImGui::SmallButton("B>###nq_bookmark_next")) m_Canvas->JumpBookmark(1);
	ImGui::EndDisabled();

	ImGui::SameLine();
	bool minimap = m_Canvas->MinimapVisible();
	if (ImGui::Checkbox("Minimap###nq_minimap", &minimap)) m_Canvas->SetMinimapVisible(minimap);
	if (!m_ShowFind) return;

	if (m_FocusFind) { ImGui::SetKeyboardFocusHere(); m_FocusFind = false; }
	ImGui::SetNextItemWidth(_max(180.f, _min(440.f, ImGui::GetContentRegionAvail().x - 220.f)));
	const bool submit = ImGui::InputTextWithHint("##nq_find", "Find id, kind, params, actions, comments", m_Find, sizeof(m_Find), ImGuiInputTextFlags_EnterReturnsTrue);
	const bool edited = ImGui::IsItemEdited();
	const bool active = ImGui::IsItemActive();
	if (edited) RefreshFind(true);
	if (submit) NavigateFind(ImGui::GetIO().KeyShift ? m_FindIndex - 1 : m_FindIndex + 1);
	if (active && ImGui::IsKeyPressed(ImGuiKey_Escape)) m_ShowFind = false;

	ImGui::SameLine();
	ImGui::BeginDisabled(m_FindResults.empty());
	if (ImGui::SmallButton("Prev###nq_find_prev")) NavigateFind(m_FindIndex - 1);
	ImGui::SameLine();
	if (ImGui::SmallButton("Next###nq_find_next")) NavigateFind(m_FindIndex + 1);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (m_FindResults.empty()) ImGui::TextDisabled("0 results");
	else ImGui::TextDisabled("%d/%d", m_FindIndex >= 0 ? m_FindIndex + 1 : 0, (int)m_FindResults.size());
	ImGui::SameLine();
	const char* preview = m_FindIndex >= 0 && m_FindIndex < (int)m_FindResults.size() ? m_FindResults[m_FindIndex].node.c_str() : "Results";
	ImGui::SetNextItemWidth(150.f);
	if (ImGui::BeginCombo("##nq_find_results", preview))
	{
		ImGuiListClipper clipper;
		clipper.Begin((int)m_FindResults.size());
		if (m_FindIndex >= 0) clipper.IncludeItemByIndex(m_FindIndex);
		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
			{
				xr_string label = m_FindResults[i].node + " - " + m_FindResults[i].title;
				if (ImGui::Selectable(label.c_str(), i == m_FindIndex)) NavigateFind(i);
				if (i == m_FindIndex) ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("x###nq_find_close")) m_ShowFind = false;
}

void UIQuestGraphWindow::DrawToolbar()
{
	if (ImGui::Button("Save"))				Save();
	ImGui::SameLine();
	if (ImGui::Button("Reload"))			{ xr_string err; m_Message = m_Doc->Reload(err) ? "reloaded" : "reload failed: " + err; }
	ImGui::SameLine();
	if (ImGui::Button("Validate"))			{ m_Doc->Validate(); m_ShowProblems = true; }
	ImGui::SameLine();
	if (ImGui::Button("Layout"))			{ m_Doc->Layout(false); m_Canvas->RequestFrameAll(); }
	ImGui::SameLine();
	if (ImGui::Button("Frame all"))			m_Canvas->RequestFrameAll();
	ImGui::SameLine();
	if (ImGui::Button("-"))					m_Canvas->ZoomBy(-1);
	ImGui::SameLine();
	ImGui::Text("%d%%", int(m_Canvas->Zoom() * 100.f + 0.5f));
	ImGui::SameLine();
	if (ImGui::Button("+"))					m_Canvas->ZoomBy(1);
	ImGui::SameLine();
	ImGui::TextDisabled("| undo %d  redo %d | catalog: %s v%d", m_Doc->UndoDepth(), m_Doc->RedoDepth(), NqCatalog::Source(), NqCatalog::Version());
	if (!m_Message.empty()) { ImGui::SameLine(); ImGui::TextDisabled("| %s", m_Message.c_str()); }
}

float UIQuestGraphWindow::SplitWidth(float body_w) const
{
	// the inspector may take nearly the whole tab - its description and lua boxes
	// are unusable narrow; the canvas only keeps a strip. A tab too narrow for
	// both minima is split by the fraction instead, so neither pane vanishes.
	const float em = ImGui::GetFrameHeight();
	const float lo = _min(s_InspectorMinEm * em, body_w * 0.5f);
	const float hi = _max(body_w - SplitGrab() - s_CanvasMinEm * em, lo);
	return _max(lo, _min(m_SplitFrac * body_w, hi));
}

//------------------------------------------------------------------------------
// canvas | inspector handle, same shape as UIContentBrowser::DrawSplitter. An
// InvisibleButton is the whole handle: it takes the mouse capture, so the drag
// keeps working while the cursor runs ahead of the pane, and it swallows the
// click so neither child sees it. The grab area is wider than the drawn line -
// a 1px target is unusable.
//------------------------------------------------------------------------------
void UIQuestGraphWindow::DrawSplitter(float body_w, float body_h)
{
	const float grab = SplitGrab();

	ImGui::SameLine(0.f, 0.f);
	const ImVec2 top = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##nq_split", ImVec2(grab, body_h > 0.f ? body_h : 1.f));
	const bool active	= ImGui::IsItemActive();
	const bool hovered	= ImGui::IsItemHovered();

	// double click puts it back: the way out when the bar was dragged against an
	// edge and the pane it belongs to is no longer reachable
	if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
	{
		m_SplitFrac = float(s_SplitDefault) / 1000.f;
		SaveSplit(m_SplitFrac);
	}
	else if (active)
	{
		m_SplitFrac = (SplitWidth(body_w) - ImGui::GetIO().MouseDelta.x) / body_w;
		// re-clamped on the spot: a drag pushed past a minimum must not bank up
		// invisible width, or coming back does nothing until the debt is paid off
		m_SplitFrac = SplitWidth(body_w) / body_w;
		SaveSplit(m_SplitFrac);
	}
	if (active || hovered)
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

	// visible grip: separator tint at rest, the usual accent while dragged, and it
	// widens to the whole grab area under the cursor - the bar was easy to miss
	const ImU32 col = ImGui::GetColorU32(active	 ? ImGuiCol_SeparatorActive
									   : hovered ? ImGuiCol_SeparatorHovered
												 : ImGuiCol_Separator);
	const float x = top.x + grab * 0.5f;
	const float half = (active || hovered) ? grab * 0.5f : 1.f;
	ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x - half, top.y),
											  ImVec2(x + half, top.y + ImGui::GetItemRectSize().y), col);
	if (hovered && !active) ImGui::SetTooltip("Drag to resize, double-click to reset");

	ImGui::SameLine(0.f, 0.f);
}

float UIQuestGraphWindow::ProblemListHeight() const
{
	// as tall as the rows need and no taller, the rest scrolls: an empty strip has
	// no business eating a fixed slice of the canvas ("none" is still one row)
	const int rows = _max(_min((int)m_Doc->problems.size(), 4), 1);
	return rows * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y * 2.f;
}

float UIQuestGraphWindow::ProblemsHeight() const
{
	const ImGuiStyle& st = ImGui::GetStyle();
	float h = ImGui::GetFrameHeight() + st.ItemSpacing.y;		// the collapsing header
	if (m_ShowProblems) h += ProblemListHeight() + st.ItemSpacing.y;
	return h;
}

void UIQuestGraphWindow::DrawProblems()
{
	int e = m_Doc->ErrorCount(), w = m_Doc->WarningCount();
	ImVec4 col = e ? ImVec4(1, 0.45f, 0.4f, 1) : (w ? ImVec4(1, 0.85f, 0.4f, 1) : ImVec4(0.6f, 0.9f, 0.6f, 1));
	xr_string head = NqUtil::Format("Problems: %d error(s), %d warning(s)###nq_problems", e, w);
	ImGui::PushStyleColor(ImGuiCol_Text, col);
	bool open = ImGui::CollapsingHeader(head.c_str(), m_ShowProblems ? ImGuiTreeNodeFlags_DefaultOpen : 0);
	ImGui::PopStyleColor();
	m_ShowProblems = open;
	if (!open) return;
	ImGui::BeginChild("nq_problem_list", ImVec2(0, ProblemListHeight()), true);
	for (u32 i = 0; i < m_Doc->problems.size(); ++i)
	{
		const SNqProblem& p = m_Doc->problems[i];
		ImGui::PushID((int)i);
		ImGui::PushStyleColor(ImGuiCol_Text, p.IsError() ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(1, 0.85f, 0.4f, 1));
		const xr_string text = p.Text();
		bool clicked = ImGui::Selectable(text.c_str());
		ImGui::PopStyleColor();
		if (clicked && !p.node_id.empty())
		{
			m_Canvas->RequestFrameNode(p.node_id.c_str());
			m_Doc->sel_slot = p.slot.find("enter:") == 0 || p.slot.find("exit:") == 0 ? p.slot : "";
		}
		// a message names a node, a slot and a parameter - it is meant to be pasted
		// into a report or a chat, and reading it off the screen by hand is silly
		if (ImGui::BeginPopupContextItem("##nq_problem_ctx"))
		{
			if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(text.c_str());
			if (ImGui::MenuItem("Copy all"))
			{
				xr_string all;
				for (u32 k = 0; k < m_Doc->problems.size(); ++k)
				{
					all += m_Doc->problems[k].Text();
					all += "\r\n";
				}
				ImGui::SetClipboardText(all.c_str());
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
	if (m_Doc->problems.empty()) ImGui::TextDisabled("none");
	ImGui::EndChild();
}

void UIQuestGraphWindow::DrawClosePrompt()
{
	if (!m_AskClose) return;
	ImGui::OpenPopup("Unsaved quest###nq_close");
	if (ImGui::BeginPopupModal("Unsaved quest###nq_close", 0, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("\"%s\" has unsaved changes.", NqUtil::BaseName(m_Doc->path.c_str()).c_str());
		ImGui::Separator();
		// safe choice first and on Escape: keep editing
		if (ImGui::Button("Keep editing") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { m_AskClose = false; ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ImGui::Button("Save and close")) { Save(); if (!m_Doc->dirty) { m_CloseNow = true; ImGui::CloseCurrentPopup(); } }
		ImGui::SameLine();
		if (ImGui::Button("Discard changes")) { m_CloseNow = true; ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
}

void UIQuestGraphWindow::Draw()
{
	if (m_CloseNow) { bOpen = false; return; }
	if (m_Focus) { ImGui::SetNextWindowFocus(); m_Focus = false; }
	// a tab beside the viewport, like the model preview (docs par. 13.7)
	UI->DockNextWindowWith("Render");
	ImGui::SetNextWindowSize(ImVec2(1100.f, 700.f), ImGuiCond_FirstUseEver);
	bool open = true;
	if (!ImGui::Begin(Title().c_str(), &open))
	{
		ImGui::End();
		if (!open) RequestClose(false);
		return;
	}
	if (!open) RequestClose(false);

	// hotkeys of the tab (the canvas handles its own when hovered/focused)
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput)
	{
		ImGuiIO& io = ImGui::GetIO();
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) Save();
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F))
		{
			if (UI) UI->BlockShortCuts();
			m_ShowFind = true;
			m_FocusFind = true;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F3))
		{
			if (UI) UI->BlockShortCuts();
			NavigateFind(io.KeyShift ? m_FindIndex - 1 : m_FindIndex + 1);
		}
	}

	DrawToolbar();
	DrawFindBar();
	ImGui::Separator();

	// canvas | inspector split with a draggable bar
	ImVec2 avail = ImGui::GetContentRegionAvail();
	float body_h = _max(avail.y - ProblemsHeight(), 100.f);
	float body_w = _max(avail.x, 1.f);
	float insp_w = SplitWidth(body_w);
	float canvas_w = _max(body_w - insp_w - SplitGrab(), 1.f);

	ImGui::BeginChild("nq_canvas_child", ImVec2(canvas_w, body_h), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	m_Canvas->Draw(ImVec2(canvas_w, body_h));
	ImGui::EndChild();

	DrawSplitter(body_w, body_h);

	// the inspector fills what is left of the line. It scrolls both ways: a field
	// that does not fit must be reachable, not quietly clipped off the edge
	ImGui::BeginChild("nq_inspector_child", ImVec2(0, body_h), false, ImGuiWindowFlags_HorizontalScrollbar);
	m_Inspector->Draw(m_Canvas->TakeRenameRequest(), m_Canvas->TakeFocusAction());
	ImGui::EndChild();

	DrawProblems();
	DrawClosePrompt();
	ImGui::End();
}

void UIQuestGraphWindow::McpFind(LPCSTR raw, xr_string& out)
{
	xr_string action, query;
	NqUtil::ArgString(raw, "action", action);
	const bool has_query = NqUtil::ArgString(raw, "query", query);
	if (action.empty()) action = has_query ? "set" : "get";
	if (has_query && query.size() >= sizeof(m_Find)) { NqUtil::JsonError(out, "query is too long (max 255 UTF-8 bytes)"); return; }

	if (action == "set")
	{
		SetFindQuery(has_query ? query.c_str() : "");
		if (NqUtil::ArgBool(raw, "select", false)) NavigateFind(0);
	}
	else if (action == "next" || action == "previous")
	{
		if (has_query) SetFindQuery(query.c_str());
		NavigateFind(action == "previous" ? m_FindIndex - 1 : m_FindIndex + 1);
	}
	else if (action == "clear")
	{
		m_Find[0] = 0;
		m_ShowFind = false;
		RefreshFind(true);
	}
	else if (action == "get")
	{
		if (has_query) SetFindQuery(query.c_str());
	}
	else
	{
		NqUtil::JsonError(out, "action must be get, set, next, previous or clear");
		return;
	}

	out = "{\"ok\":true,\"path\":";
	NqUtil::JsonPath(out, NqUtil::ProjectRelative(m_Doc->path.c_str()).c_str());
	AppendFindState(out, NqUtil::ArgInt(raw, "limit", 100));
	out += "}";
}

void UIQuestGraphWindow::McpBookmarks(LPCSTR raw, xr_string& out)
{
	xr_vector<xr_string> known = m_Canvas->Bookmarks();
	for (u32 i = 0; i < known.size(); ++i)
		if (!m_Doc->quest.FindNode(known[i].c_str())) m_Canvas->SetBookmark(known[i].c_str(), false);

	xr_string action, node;
	NqUtil::ArgString(raw, "action", action);
	NqUtil::ArgString(raw, "node", node);
	if (action.empty()) action = "get";
	if (node.empty() && m_Doc->selection.size() == 1) node = m_Doc->selection[0];

	if (action == "add" || action == "remove" || action == "toggle")
	{
		if (node.empty()) { NqUtil::JsonError(out, "node is required unless exactly one node is selected"); return; }
		if (action != "remove" && !m_Doc->quest.FindNode(node.c_str())) { NqUtil::JsonError(out, "node not found"); return; }
		const bool enabled = action == "add" || (action == "toggle" && !m_Canvas->IsBookmarked(node.c_str()));
		if (!m_Canvas->SetBookmark(node.c_str(), enabled)) { NqUtil::JsonError(out, "bookmark update failed"); return; }
	}
	else if (action == "next" || action == "previous")
	{
		if (!m_Canvas->JumpBookmark(action == "previous" ? -1 : 1)) { NqUtil::JsonError(out, "no bookmarks"); return; }
	}
	else if (action == "clear") m_Canvas->ClearBookmarks();
	else if (action != "get")
	{
		NqUtil::JsonError(out, "action must be get, add, remove, toggle, next, previous or clear");
		return;
	}

	out = "{\"ok\":true,\"path\":";
	NqUtil::JsonPath(out, NqUtil::ProjectRelative(m_Doc->path.c_str()).c_str());
	out += ",\"bookmarks\":";
	NqUtil::JsonStringArray(out, m_Canvas->Bookmarks());
	out += ",\"count\":";
	NqUtil::JsonInt(out, (int)m_Canvas->Bookmarks().size());
	AppendViewState(out);
	out += "}";
}

void UIQuestGraphWindow::McpHistory(LPCSTR raw, xr_string& out)
{
	xr_string action;
	NqUtil::ArgString(raw, "action", action);
	if (action.empty()) action = "get";
	bool moved = false;
	if (action == "back") moved = m_Canvas->HistoryBack();
	else if (action == "forward") moved = m_Canvas->HistoryForward();
	else if (action == "clear") m_Canvas->ClearHistory();
	else if (action != "get")
	{
		NqUtil::JsonError(out, "action must be get, back, forward or clear");
		return;
	}

	auto append_state = [](xr_string& json, const NqCanvas::SViewState& state)
	{
		json += NqUtil::Format("{\"center\":[%.1f,%.1f],\"zoom_level\":%d,\"selected\":", state.cx, state.cy, state.zoom);
		NqUtil::JsonStringArray(json, state.selection);
		json += ",\"slot\":";
		NqUtil::JsonString(json, state.slot);
		json += "}";
	};
	out = "{\"ok\":true,\"path\":";
	NqUtil::JsonPath(out, NqUtil::ProjectRelative(m_Doc->path.c_str()).c_str());
	out += ",\"moved\":";
	NqUtil::JsonBool(out, moved);
	const xr_vector<NqCanvas::SViewState>& back = m_Canvas->BackHistory();
	const xr_vector<NqCanvas::SViewState>& forward = m_Canvas->ForwardHistory();
	out += ",\"back_count\":";
	NqUtil::JsonInt(out, (int)back.size());
	out += ",\"forward_count\":";
	NqUtil::JsonInt(out, (int)forward.size());
	out += ",\"back\":[";
	for (u32 i = 0; i < back.size(); ++i) { if (i) out += ","; append_state(out, back[i]); }
	out += "],\"forward\":[";
	for (u32 i = 0; i < forward.size(); ++i) { if (i) out += ","; append_state(out, forward[i]); }
	out += "],\"current\":";
	append_state(out, m_Canvas->CurrentView());
	out += "}";
}

void UIQuestGraphWindow::McpMinimap(LPCSTR raw, xr_string& out)
{
	xr_string action;
	NqUtil::ArgString(raw, "action", action);
	if (action.empty()) action = "get";
	if (action == "show") m_Canvas->SetMinimapVisible(true);
	else if (action == "hide") m_Canvas->SetMinimapVisible(false);
	else if (action == "toggle") m_Canvas->SetMinimapVisible(!m_Canvas->MinimapVisible());
	else if (action != "get")
	{
		NqUtil::JsonError(out, "action must be get, show, hide or toggle");
		return;
	}

	out = "{\"ok\":true,\"path\":";
	NqUtil::JsonPath(out, NqUtil::ProjectRelative(m_Doc->path.c_str()).c_str());
	out += ",\"visible\":";
	NqUtil::JsonBool(out, m_Canvas->MinimapVisible());
	float min_x, min_y, max_x, max_y;
	out += ",\"graph_bounds\":";
	if (m_Canvas->GraphBounds(min_x, min_y, max_x, max_y))
		out += NqUtil::Format("[%.1f,%.1f,%.1f,%.1f]", min_x, min_y, max_x, max_y);
	else out += "null";
	out += ",\"viewport_bounds\":";
	if (m_Canvas->ViewportBounds(min_x, min_y, max_x, max_y))
		out += NqUtil::Format("[%.1f,%.1f,%.1f,%.1f]", min_x, min_y, max_x, max_y);
	else out += "null";
	AppendViewState(out);
	out += "}";
}

//------------------------------------------------------------------------------
// registry
//------------------------------------------------------------------------------
namespace UIQuestGraph
{
	UIQuestGraphWindow* Find(LPCSTR path)
	{
		xr_string abs, err;
		if (!NqDocs::Resolve(path, abs, err)) return 0;
		xr_string key = NqUtil::PathKey(abs.c_str());
		for (u32 i = 0; i < s_Windows.size(); ++i)
			if (NqUtil::PathKey(s_Windows[i]->Doc()->path.c_str()) == key) return s_Windows[i];
		return 0;
	}

	bool Open(LPCSTR path, xr_string* err)
	{
		xr_string e;
		if (UIQuestGraphWindow* w = Find(path)) { w->Focus(); return true; }
		NqDoc* doc = NqDocs::Open(path, e);
		if (!doc)
		{
			if (err) *err = e; else ELog.DlgMsg(mtError, "Quest graph: %s", e.c_str());
			return false;
		}
		s_Windows.push_back(xr_new<UIQuestGraphWindow>(doc));
		return true;
	}

	void Update()
	{
		// tabs die with the project they belong to. The first pump of a session
		// only records the project: documents opened over MCP before this point
		// belong to it already and must not be torn down.
		xr_string proj = EditorProject::Active() ? EditorProject::Root() : "";
		if (proj != s_LastProject)
		{
			if (!s_LastProject.empty()) OnProjectChanged();
			s_LastProject = proj;
		}
		for (u32 i = 0; i < s_Windows.size(); )
		{
			UIQuestGraphWindow* w = s_Windows[i];
			// the document may have been closed without us (MCP quest_close):
			// drop the tab instead of drawing a freed NqDoc
			if (!NqDocs::IsOpen(w->Doc()))
			{
				xr_delete(w);
				s_Windows.erase(s_Windows.begin() + i);
				continue;
			}
			w->Draw();
			if (w->IsClosed())
			{
				xr_string err;
				NqDocs::Close(w->Doc()->path.c_str(), true, err);
				xr_delete(w);
				s_Windows.erase(s_Windows.begin() + i);
			}
			else ++i;
		}
	}

	void CloseAll(bool discard)
	{
		for (u32 i = 0; i < s_Windows.size(); ++i) s_Windows[i]->RequestClose(discard);
	}

	void OnProjectChanged()
	{
		// the tabs die with the project, the work in them does not: every dirty
		// document is written back before anything is destroyed. No prompt - this
		// runs from the draw loop, and MCP / -nodlg runs must never stall on a
		// modal (the file names of what was saved go to the log instead).
		const int saved = NqDocs::SaveDirty();
		if (saved) Msg("* [nq] %d unsaved quest(s) saved before leaving the project", saved);
		for (u32 i = 0; i < s_Windows.size(); ++i) xr_delete(s_Windows[i]);
		s_Windows.clear();
		NqDocs::CloseAll();
	}

	int Count() { return (int)s_Windows.size(); }

	static UIQuestGraphWindow* OpenMcpWindow(LPCSTR raw, xr_string& out)
	{
		xr_string path;
		if (!NqUtil::ArgString(raw, "path", path) || path.empty()) { NqUtil::JsonError(out, "path is required"); return 0; }
		xr_string err;
		if (!Open(path.c_str(), &err)) { NqUtil::JsonError(out, err.c_str()); return 0; }
		UIQuestGraphWindow* window = Find(path.c_str());
		if (!window) { NqUtil::JsonError(out, "window not found"); return 0; }
		window->Focus();
		return window;
	}

	void McpClose(LPCSTR raw, xr_string& out)
	{
		xr_string path;
		if (!NqUtil::ArgString(raw, "path", path) || path.empty()) { NqUtil::JsonError(out, "missing 'path' (project-relative, e.g. quests/wolf_debt.nqasset)"); return; }
		NqDoc* doc = NqDocs::Get(path.c_str());
		if (!doc) { NqUtil::JsonError(out, "not open"); return; }
		const bool discard = NqUtil::ArgBool(raw, "discard", false);
		if (doc->dirty && !discard) { NqUtil::JsonError(out, "unsaved"); return; }
		const xr_string absolute = doc->path;

		// Canvas and inspector retain the document pointer, so every matching view must die first.
		for (u32 i = 0; i < s_Windows.size(); )
		{
			if (s_Windows[i]->Doc() != doc) { ++i; continue; }
			xr_delete(s_Windows[i]);
			s_Windows.erase(s_Windows.begin() + i);
		}

		xr_string err;
		if (!NqDocs::Close(absolute.c_str(), discard, err)) { NqUtil::JsonError(out, err); return; }
		out = "{\"ok\":true}";
	}

	void McpView(LPCSTR raw, xr_string& out)
	{
		UIQuestGraphWindow* w = OpenMcpWindow(raw, out);
		if (!w) return;
		NqCanvas* c = w->Canvas();
		string_path frame = "";
		XFinedMCP::GetArg(raw, "frame", frame, sizeof(frame));
		int zoom = NqUtil::ArgInt(raw, "zoom_level", -1);
		float cx, cy;
		bool has_c = NqUtil::ArgFloat(raw, "cx", cx) && NqUtil::ArgFloat(raw, "cy", cy);
		string_path slot = "";
		XFinedMCP::GetArg(raw, "slot", slot, sizeof(slot));
		// Reject validator-only slots before changing any part of the view.
		if (slot[0] && 0 != strcmp(slot, "none"))
		{
			LPCSTR digits = 0;
			if (0 == strncmp(slot, "enter:", 6))		digits = slot + 6;
			else if (0 == strncmp(slot, "exit:", 5))	digits = slot + 5;
			bool ok = digits && digits[0];
			for (LPCSTR p = digits; ok && *p; ++p) if (*p < '0' || *p > '9') ok = false;
			if (!ok) { NqUtil::JsonError(out, "slot must be \"enter:N\", \"exit:N\" or \"none\""); return; }
		}

		c->BeginNavigationBatch();
		if (frame[0] && 0 == strcmp(frame, "all")) c->RequestFrameAll();
		else if (frame[0]) c->RequestFrameNode(frame);
		if (zoom >= 0) c->SetZoom(zoom);
		if (has_c) c->Center(cx, cy);
		// "enter:2" / "exit:0" opens that action in the inspector; framing a
		// node clears the slot, so this comes last
		if (slot[0])
		{
			if (0 == strcmp(slot, "none")) w->Doc()->sel_slot.clear();
			else w->Doc()->sel_slot = slot;
		}
		c->EndNavigationBatch();
		out = "{\"ok\":true,\"path\":";
		NqUtil::JsonPath(out, w->Doc()->path.c_str());
		out += NqUtil::Format(",\"zoom_level\":%d,\"zoom\":%.3f,\"center\":[%.1f,%.1f],\"nodes\":%d,\"open_tabs\":%d,\"slot\":",
			w->Doc()->zoom_idx, c->Zoom(), w->Doc()->view_cx, w->Doc()->view_cy, int(w->Doc()->quest.nodes.size()), Count());
		NqUtil::JsonString(out, w->Doc()->sel_slot);
		out += ",\"selected\":";
		NqUtil::JsonStringArray(out, w->Doc()->selection);
		out += ",\"pending\":";
		NqUtil::JsonBool(out, c->FramePending());
		out += "}";
	}

	void McpFind(LPCSTR raw, xr_string& out)
	{
		if (UIQuestGraphWindow* window = OpenMcpWindow(raw, out)) window->McpFind(raw, out);
	}

	void McpBookmarks(LPCSTR raw, xr_string& out)
	{
		if (UIQuestGraphWindow* window = OpenMcpWindow(raw, out)) window->McpBookmarks(raw, out);
	}

	void McpHistory(LPCSTR raw, xr_string& out)
	{
		if (UIQuestGraphWindow* window = OpenMcpWindow(raw, out)) window->McpHistory(raw, out);
	}

	void McpMinimap(LPCSTR raw, xr_string& out)
	{
		if (UIQuestGraphWindow* window = OpenMcpWindow(raw, out)) window->McpMinimap(raw, out);
	}
}
