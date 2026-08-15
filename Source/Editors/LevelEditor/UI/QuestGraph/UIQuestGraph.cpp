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
}

//------------------------------------------------------------------------------
// window
//------------------------------------------------------------------------------
UIQuestGraphWindow::UIQuestGraphWindow(NqDoc* doc) : m_Doc(doc)
{
	m_Canvas = xr_new<NqCanvas>(doc);
	m_Inspector = xr_new<NqInspector>(doc);
	m_Split = 340.f;
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
	ImGui::BeginChild("nq_problem_list", ImVec2(0, 96.f), true);
	for (u32 i = 0; i < m_Doc->problems.size(); ++i)
	{
		const SNqProblem& p = m_Doc->problems[i];
		ImGui::PushStyleColor(ImGuiCol_Text, p.IsError() ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(1, 0.85f, 0.4f, 1));
		bool clicked = ImGui::Selectable(p.Text().c_str());
		ImGui::PopStyleColor();
		if (clicked && !p.node_id.empty())
		{
			m_Canvas->RequestFrameNode(p.node_id.c_str());
			m_Doc->sel_slot = p.slot.find("enter:") == 0 || p.slot.find("exit:") == 0 ? p.slot : "";
		}
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
	float problems_h = 26.f + (m_ShowProblems ? 100.f : 0.f);
	float body_h = _max(avail.y - problems_h, 100.f);
	float insp_w = _max(220.f, _min(m_Split, avail.x - 300.f));
	float canvas_w = _max(200.f, avail.x - insp_w - 8.f);

	ImGui::BeginChild("nq_canvas_child", ImVec2(canvas_w, body_h), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	m_Canvas->Draw(ImVec2(canvas_w, body_h));
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::InvisibleButton("nq_splitter", ImVec2(8.f, body_h));
	if (ImGui::IsItemActive()) m_Split -= ImGui::GetIO().MouseDelta.x;
	if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	ImGui::SameLine();

	ImGui::BeginChild("nq_inspector_child", ImVec2(0, body_h), false);
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
