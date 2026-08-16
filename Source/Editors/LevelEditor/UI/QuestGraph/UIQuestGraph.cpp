#include "stdafx.h"
#include "UIQuestGraph.h"
#include "NqCanvas.h"
#include "NqInspector.h"
#include "../../../XrECore/Editor/Nq/NqUtil.h"
#include "../../../XrECore/Editor/Nq/NqCatalog.h"
#include "../../../XrECore/Editor/Nq/NqLayout.h"

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
	}

	DrawToolbar();
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

	void McpView(LPCSTR raw, xr_string& out)
	{
		string_path path = "";
		XFinedMCP::GetArg(raw, "path", path, sizeof(path));
		if (!path[0]) { NqUtil::JsonError(out, "path is required"); return; }
		xr_string err;
		if (!Open(path, &err)) { NqUtil::JsonError(out, err.c_str()); return; }
		UIQuestGraphWindow* w = Find(path);
		if (!w) { NqUtil::JsonError(out, "window not found"); return; }
		w->Focus();
		NqCanvas* c = w->Canvas();
		string_path frame = "";
		XFinedMCP::GetArg(raw, "frame", frame, sizeof(frame));
		int zoom = NqUtil::ArgInt(raw, "zoom_level", -1);
		float cx, cy;
		bool has_c = NqUtil::ArgFloat(raw, "cx", cx) && NqUtil::ArgFloat(raw, "cy", cy);
		if (frame[0] && 0 == strcmp(frame, "all")) c->RequestFrameAll();
		else if (frame[0]) c->RequestFrameNode(frame);
		if (zoom >= 0) c->SetZoom(zoom);
		if (has_c) c->Center(cx, cy);
		// "enter:2" / "exit:0" opens that action in the inspector; framing a
		// node clears the slot, so this comes last
		string_path slot = "";
		XFinedMCP::GetArg(raw, "slot", slot, sizeof(slot));
		if (slot[0])
		{
			if (0 == strcmp(slot, "none")) w->Doc()->sel_slot.clear();
			else
			{
				// only "enter:N" / "exit:N" mean anything to the inspector; anything
				// else (a validator slot like "cond:1" or "param:npc", say) would be
				// silently treated as on_enter and edits would land in the wrong list
				LPCSTR digits = 0;
				if (0 == strncmp(slot, "enter:", 6))		digits = slot + 6;
				else if (0 == strncmp(slot, "exit:", 5))	digits = slot + 5;
				bool ok = digits && digits[0];
				for (LPCSTR p = digits; ok && *p; ++p) if (*p < '0' || *p > '9') ok = false;
				if (!ok) { NqUtil::JsonError(out, "slot must be \"enter:N\", \"exit:N\" or \"none\""); return; }
				w->Doc()->sel_slot = slot;
			}
		}
		out = "{\"ok\":true,\"path\":";
		NqUtil::JsonPath(out, w->Doc()->path.c_str());
		out += NqUtil::Format(",\"zoom_level\":%d,\"zoom\":%.3f,\"center\":[%.1f,%.1f],\"nodes\":%d,\"open_tabs\":%d,\"slot\":",
			w->Doc()->zoom_idx, c->Zoom(), w->Doc()->view_cx, w->Doc()->view_cy, int(w->Doc()->quest.nodes.size()), Count());
		NqUtil::JsonString(out, w->Doc()->sel_slot);
		out += ",\"selected\":";
		NqUtil::JsonStringArray(out, w->Doc()->selection);
		out += "}";
	}
}
