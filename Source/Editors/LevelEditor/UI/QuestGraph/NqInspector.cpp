#include "stdafx.h"
#include "NqInspector.h"
#include "../../../XrECore/Editor/Nq/NqLua.h"
#include "../../../XrECore/Editor/Nq/NqPickers.h"
#include "../../../XrECore/Editor/Nq/NqUtil.h"

namespace
{
	// text widgets over xr_string: the buffer is refilled from the model every
	// frame and written back on every change, so ImGui's own edit state and the
	// staged copy never diverge. It grows with the value - a fixed buffer would
	// silently truncate long texts and Lua bodies the moment they are edited.
	// ImGui hides everything after "##" in a widget label, plain text does not -
	// labels that carry an id suffix must be trimmed before they are printed
	LPCSTR Shown(LPCSTR label, xr_string& tmp)
	{
		LPCSTR h = label ? strstr(label, "##") : 0;
		if (!h) return label ? label : "";
		tmp.assign(label, h - label);
		return tmp.c_str();
	}

	bool HiddenLabel(LPCSTR label) { return !label || (label[0] == '#' && label[1] == '#'); }

	// ImGui's default item width is a flat 65% of the panel: a field stays the
	// same size however wide the author drags the inspector, and whatever the row
	// draws after it - the label itself, a "default" or "..." button - is pushed
	// past the right edge and simply never appears. Sizing every field as
	// "everything except what still has to fit" is what makes the panel react to
	// its own width.
	float LabelRoom(LPCSTR label)
	{
		if (HiddenLabel(label)) return 0.f;
		xr_string tmp;
		LPCSTR shown = Shown(label, tmp);
		if (!shown[0]) return 0.f;
		return ImGui::CalcTextSize(shown).x + ImGui::GetStyle().ItemInnerSpacing.x;
	}

	float ButtonRoom(LPCSTR text)
	{
		const ImGuiStyle& st = ImGui::GetStyle();
		return ImGui::CalcTextSize(text).x + st.FramePadding.x * 2.f + st.ItemSpacing.x;
	}

	// a combo has to fit its widest entry plus the arrow square, otherwise the
	// author reads "game_minu" and has to open the list to know what is selected.
	// Hardcoded pixel widths cannot do this: the font is scaled by the monitor DPI.
	float ComboRoom(const xr_vector<xr_string>& items, LPCSTR extra = 0)
	{
		float w = extra ? ImGui::CalcTextSize(extra).x : 0.f;
		for (u32 i = 0; i < items.size(); ++i) w = _max(w, ImGui::CalcTextSize(items[i].c_str()).x);
		return w + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.f;
	}

	// a text box tall enough for what is in it: one-liners stay compact, a Lua
	// body or a long briefing gets room, and past the cap it scrolls instead of
	// pushing everything else out of the panel
	float FitRows(const xr_string& s, int min_rows, int max_rows)
	{
		int rows = 1;
		for (u32 i = 0; i < s.size(); ++i) if (s[i] == '\n') ++rows;
		if (rows < min_rows) rows = min_rows;
		if (rows > max_rows) rows = max_rows;
		return ImGui::GetTextLineHeight() * float(rows) + ImGui::GetStyle().FramePadding.y * 2.f;
	}

	// reserve: pixels the caller still needs on the right of the field (a trailing
	// button), on top of the label
	bool InputStr(LPCSTR label, xr_string& s, bool multiline = false, float height = 0.f, float reserve = 0.f)
	{
		static xr_vector<char> buf;
		const size_t need = s.size() + 4096;
		if (buf.size() < need) buf.resize(need);
		memcpy(&buf[0], s.c_str(), s.size() + 1);
		bool changed;
		if (multiline)
		{
			// a box that spans the panel leaves no room for a right-hand label, so
			// ImGui draws it off the edge - print it above the box instead
			xr_string tmp;
			if (!HiddenLabel(label)) ImGui::TextDisabled("%s", Shown(label, tmp));
			xr_string id = xr_string("##ml") + (label ? label : "");
			changed = ImGui::InputTextMultiline(id.c_str(), &buf[0], buf.size(),
				ImVec2(-1.f, height > 0.f ? height : FitRows(s, 4, 16)),
				ImGuiInputTextFlags_AllowTabInput);
		}
		else
		{
			const float room = LabelRoom(label) + reserve;
			if (room > 0.f) ImGui::SetNextItemWidth(-room);
			changed = ImGui::InputText(label, &buf[0], buf.size());
		}
		if (changed) s = &buf[0];
		return changed;
	}

	bool ComboStr(LPCSTR label, xr_string& cur, const xr_vector<xr_string>& items, LPCSTR empty_label = 0, float reserve = 0.f)
	{
		bool changed = false;
		LPCSTR preview = cur.empty() ? (empty_label ? empty_label : "") : cur.c_str();
		const float room = LabelRoom(label) + reserve;
		if (room > 0.f) ImGui::SetNextItemWidth(-room);
		if (ImGui::BeginCombo(label, preview))
		{
			if (empty_label && ImGui::Selectable(empty_label, cur.empty())) { cur.clear(); changed = true; }
			for (u32 i = 0; i < items.size(); ++i)
				if (ImGui::Selectable(items[i].c_str(), cur == items[i])) { cur = items[i]; changed = true; }
			ImGui::EndCombo();
		}
		return changed;
	}

	xr_vector<xr_string> Items(LPCSTR a, LPCSTR b = 0, LPCSTR c = 0, LPCSTR d = 0, LPCSTR e = 0)
	{
		xr_vector<xr_string> v;
		if (a) v.push_back(a); if (b) v.push_back(b); if (c) v.push_back(c); if (d) v.push_back(d); if (e) v.push_back(e);
		return v;
	}

	// which single key of a "mode table" is set (story/ref/profile/community/...)
	xr_string ModeOf(const SNqValue& v, const xr_vector<xr_string>& modes)
	{
		if (!v.IsTable()) return "";
		for (u32 i = 0; i < modes.size(); ++i) if (v.Has(modes[i].c_str())) return modes[i];
		return "";
	}

	bool NodeEquals(const SNqNode& a, const SNqNode& b)
	{
		SNqValue va, vb;
		NqLua::NodeToValue(a, va);
		NqLua::NodeToValue(b, vb);
		return va.Equals(vb) && a.has_pos == b.has_pos;
	}

	// A variable is named from two shapes: typed structs at node level, and plain
	// tables once a condition nests inside another one's params (any{of=...},
	// cases). Renaming has to reach both or a rename silently orphans references.
	void RenameVarInValue(SNqValue& v, LPCSTR from, LPCSTR to)
	{
		if (!v.IsTable()) return;
		SNqValue* k = v.Get("kind");
		if (k && k->IsString() && (k->s == "var" || k->s == "var.set" || k->s == "var.add"))
		{
			SNqValue* nm = v.Get("name");
			if (nm && nm->IsString() && nm->s == from) *nm = SNqValue::String(to);
		}
		for (u32 i = 0; i < v.vals.size(); ++i) RenameVarInValue(v.vals[i], from, to);
		for (u32 i = 0; i < v.arr.size(); ++i)  RenameVarInValue(v.arr[i], from, to);
	}

	void RenameVarInConds(xr_vector<SNqCond>& conds, LPCSTR from, LPCSTR to)
	{
		for (u32 i = 0; i < conds.size(); ++i)
		{
			if (conds[i].kind == "var")
			{
				SNqValue* nm = conds[i].params.Get("name");
				if (nm && nm->IsString() && nm->s == from) *nm = SNqValue::String(to);
			}
			RenameVarInValue(conds[i].params, from, to);
		}
	}

	// document-scoped ids are ASCII identifiers, so folding the ASCII range is the
	// whole job here; captions from the game go through NqPickers, which folds
	// Cyrillic as well
	bool ContainsNoCaseAscii(const xr_string& hay, LPCSTR needle)
	{
		if (!needle || !needle[0]) return true;
		xr_string h = hay, n = needle;
		for (u32 i = 0; i < h.size(); ++i) h[i] = char(tolower((u8)h[i]));
		for (u32 i = 0; i < n.size(); ++i) n[i] = char(tolower((u8)n[i]));
		return 0 != strstr(h.c_str(), n.c_str());
	}

	// The catalog states defaults as text, so the comparison is made in the type
	// the widget actually produced rather than by re-printing the value.
	bool EqualsDefault(const NqCatalog::SParam& p, const SNqValue& v)
	{
		if (!p.has_default || v.IsNil()) return false;
		if (v.IsBool())		return p.def == (v.b ? "true" : "false");
		if (v.IsNumber())	return !p.def.empty() && atof(p.def.c_str()) == v.n;
		if (v.IsString())	return p.def == v.s;
		return false;
	}

	void SplitSlot(const xr_string& sel, xr_string& slot, int& index)
	{
		slot.clear(); index = -1;
		LPCSTR c = strchr(sel.c_str(), ':');
		if (!c) return;
		slot.assign(sel.c_str(), c - sel.c_str());
		index = atoi(c + 1);
	}
}

NqInspector::NqInspector(NqDoc* doc) : m_Doc(doc)
{
	m_NodeRev = m_QuestRev = u32(-1);
	m_NodeDirty = m_QuestDirty = false;
	m_FocusRename = m_FocusAction = false;
	m_Search[0] = 0;
	m_VarsFrac = 0.30f;
	m_ParamsCtx = 0;
	m_OpenTaskRename = false;
	m_OpenTaskReferences = false;
	m_TaskReferencesComplete = true;
	m_TaskReferencesGeneration = 0;
	m_ProjectQuestIdsSerial = 0;
	if (CLevelPreferences* prefs = dynamic_cast<CLevelPreferences*>(EPrefs))
		m_VarsFrac = float(_max(_min(prefs->QuestVarsSplit, 800u), 100u)) / 1000.f;
}

//------------------------------------------------------------------------------
// staging
//------------------------------------------------------------------------------
void NqInspector::SyncNode()
{
	xr_string sel = m_Doc->selection.size() == 1 ? m_Doc->selection[0] : "";
	if (sel != m_NodeId || m_NodeRev != m_Doc->revision)
	{
		// a pending edit reaches the document before the staged copy is
		// refilled - the document moving under us (MCP, layout, canvas drag)
		// must not swallow what the user typed
		if (m_NodeDirty) CommitNode();
		m_NodeId = sel;
		m_NodeRev = m_Doc->revision;
		m_NodeDirty = false;
		const SNqNode* n = sel.empty() ? 0 : m_Doc->quest.FindNode(sel.c_str());
		if (n) m_Node = *n; else m_Node = SNqNode();
	}
}

void NqInspector::CommitNode()
{
	if (!m_NodeDirty || m_NodeId.empty()) return;
	const SNqNode* cur = m_Doc->quest.FindNode(m_NodeId.c_str());
	m_NodeDirty = false;
	if (!cur || NodeEquals(*cur, m_Node)) return;
	SNqNode staged = m_Node;
	xr_string id = m_NodeId;
	m_Doc->Edit([&](SNqQuest& q)
	{
		SNqNode* n = q.FindNode(id.c_str());
		if (!n) return;
		// the canvas owns the coordinates; the inspector never edits them and
		// must not write a stale copy back over a drag or an auto layout
		const Fvector2 keep_pos = n->pos;
		const bool keep_has = n->has_pos;
		*n = staged;
		n->pos = keep_pos;
		n->has_pos = keep_has;
	});
	m_NodeRev = m_Doc->revision;
}

void NqInspector::SyncQuest()
{
	if (m_QuestRev != m_Doc->revision)
	{
		if (m_QuestDirty) CommitQuest();
		m_Quest = m_Doc->quest;
		m_Quest.nodes.clear();		// only the header is staged here
		m_QuestRev = m_Doc->revision;
		m_QuestDirty = false;
	}
}

void NqInspector::CommitQuest()
{
	if (!m_QuestDirty) return;
	m_QuestDirty = false;
	SNqQuest h = m_Quest;
	m_Doc->Edit([&](SNqQuest& q)
	{
		q.id = h.id; q.title = h.title; q.activation = h.activation;
		q.vars = h.vars; q.tasks = h.tasks;
	});
	m_QuestRev = m_Doc->revision;
}

//------------------------------------------------------------------------------
// frame
//------------------------------------------------------------------------------
void NqInspector::Draw(bool focus_rename, bool focus_action)
{
	m_FocusRename |= focus_rename;
	m_FocusAction |= focus_action;
	SyncNode();
	SyncQuest();

	xr_string slot; int index;
	SplitSlot(m_Doc->sel_slot, slot, index);
	const bool has_action = !m_NodeId.empty() && !slot.empty();

	// tasks, conditions and per-language texts nest three levels deep, and the
	// stock 21px indent is charged to the widgets too, not just to their labels -
	// a third of a narrow panel would be spent on empty margin
	ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, ImGui::GetStyle().IndentSpacing * 0.5f);

	// The variables belong to the quest, not to whatever node happens to be
	// selected, so they get a pane of their own under the node instead of hiding
	// in the header that only shows when nothing is selected.
	const ImGuiStyle& st = ImGui::GetStyle();
	const float total	= ImGui::GetContentRegionAvail().y;
	const float grab	= ImGui::GetFrameHeight() * 0.35f + st.ItemSpacing.y;
	const float row		= ImGui::GetTextLineHeightWithSpacing();
	const float vars_min = _min(row * 4.f, total * 0.25f);
	const float props_min = _min(row * 6.f, total * 0.35f);
	const float vars_h	= _max(vars_min, _min(m_VarsFrac * total, _max(total - grab - props_min, vars_min)));
	const float props_h	= _max(row, total - grab - vars_h);
	const float upper	= has_action ? props_h * 0.6f : props_h;

	ImGui::BeginChild("nq_insp_top", ImVec2(0, upper), true);
	if (m_NodeId.empty())	DrawQuestSection();
	else					DrawNodeSection();
	ImGui::EndChild();

	if (has_action)
	{
		ImGui::BeginChild("nq_insp_bottom", ImVec2(0, _max(row, props_h - upper - st.ItemSpacing.y)), true);
		DrawActionSection();
		ImGui::EndChild();
	}

	DrawHSplitter("##nq_vars_split", m_VarsFrac, total);

	ImGui::BeginChild("nq_insp_vars", ImVec2(0, 0), true);
	DrawVarsSection();
	ImGui::EndChild();

	ImGui::PopStyleVar();

	// commit staged edits once the user lets go of the widget
	if (!ImGui::IsAnyItemActive())
	{
		if (m_NodeDirty)  CommitNode();
		if (m_QuestDirty) CommitQuest();
	}
}

//------------------------------------------------------------------------------
// quest header
//------------------------------------------------------------------------------
void NqInspector::DrawQuestSection()
{
	ImGui::TextDisabled("Quest");
	ImGui::Separator();
	bool ch = false;
	xr_string rename_requested;
	xr_string references_requested;
	ch |= InputStr("id", m_Quest.id);
	ch |= DrawText("title", m_Quest.title, false);
	{
		xr_vector<xr_string> acts = Items("auto", "manual");
		ch |= ComboStr("activation", m_Quest.activation, acts);
	}

	if (ImGui::CollapsingHeader("Tasks", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::PushID("tasks");
		for (u32 i = 0; i < m_Quest.tasks.size(); ++i)
		{
			SNqTask& t = m_Quest.tasks[i];
			ImGui::PushID((int)i);
			bool open = ImGui::TreeNodeEx("##task", ImGuiTreeNodeFlags_DefaultOpen, "%s", t.id.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("references")) references_requested = t.id;
			ImGui::SameLine();
			if (ImGui::SmallButton("rename")) rename_requested = t.id;
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) { m_Quest.tasks.erase(m_Quest.tasks.begin() + i); ch = true; if (open) ImGui::TreePop(); ImGui::PopID(); break; }
			if (open)
			{
				ImGui::TextDisabled("id");
				ImGui::SameLine();
				ImGui::TextUnformatted(t.id.c_str());
				ch |= DrawText("title##t", t.title, false);
				ch |= DrawText("descr##t", t.descr, true);
				xr_vector<xr_string> types = Items("additional", "storyline");
				ch |= ComboStr("type##t", t.type, types);
				ch |= DrawNpcRef("target##t", t.target, true, false);
				ch |= InputStr("icon##t", t.icon);

				// The steps of the task. Each is its own line in the PDA with its own
				// marker, which is how one task covers "collect X, then Y, then report"
				// instead of needing a quest apiece.
				ImGui::Separator();
				ImGui::TextDisabled("objectives (%d)", (int)t.objectives.size());
				for (u32 j = 0; j < t.objectives.size(); ++j)
				{
					SNqObjective& o = t.objectives[j];
					ImGui::PushID((int)j);
					const bool oopen = ImGui::TreeNodeEx("##obj", ImGuiTreeNodeFlags_DefaultOpen, "%u. %s",
						j + 1, o.id.empty() ? "(no id)" : o.id.c_str());
					ImGui::SameLine();
					// order is what the engine numbers them by, so it is worth moving
					ImGui::BeginDisabled(j == 0);
					if (ImGui::SmallButton("^")) { std::swap(t.objectives[j], t.objectives[j - 1]); ch = true; }
					ImGui::EndDisabled();
					ImGui::SameLine();
					ImGui::BeginDisabled(j + 1 >= t.objectives.size());
					if (ImGui::SmallButton("v")) { std::swap(t.objectives[j], t.objectives[j + 1]); ch = true; }
					ImGui::EndDisabled();
					ImGui::SameLine();
					if (ImGui::SmallButton("x"))
					{
						t.objectives.erase(t.objectives.begin() + j);
						ch = true;
						if (oopen) ImGui::TreePop();
						ImGui::PopID();
						break;
					}
					if (oopen)
					{
						ch |= InputStr("id##o", o.id);
						ch |= DrawText("title##o", o.title, false);
						ch |= DrawText("descr##o", o.descr, true);
						ch |= DrawNpcRef("target##o", o.target, true, false);
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
				if (ImGui::SmallButton("+ objective"))
				{
					SNqObjective o;
					for (u32 number = 1;; ++number)
					{
						o.id = NqUtil::Format("step%u", number);
						if (!t.FindObjective(o.id.c_str())) break;
					}
					t.objectives.push_back(o);
					ch = true;
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (ImGui::SmallButton("+ task"))
		{
			SNqTask t;
			for (u32 number = 1;; ++number)
			{
				t.id = NqUtil::Format("task%u", number);
				bool used = false;
				for (u32 i = 0; i < m_Quest.tasks.size(); ++i)
					if (m_Quest.tasks[i].id == t.id) { used = true; break; }
				if (!used) break;
			}
			t.type = "additional";
			t.title = SNqValue::String(""); t.descr = SNqValue::String("");
			m_Quest.tasks.push_back(t); ch = true;
		}
		ImGui::PopID();
	}
	if (ch) m_QuestDirty = true;
	if (!references_requested.empty()) BeginTaskReferences(references_requested.c_str());
	if (!rename_requested.empty()) BeginTaskRename(rename_requested.c_str());
	DrawTaskRename();
	DrawTaskReferences();

	ImGui::Separator();
	ImGui::TextDisabled("%d node(s), %d error(s), %d warning(s)", int(m_Doc->quest.nodes.size()), m_Doc->ErrorCount(), m_Doc->WarningCount());
}

void NqInspector::BeginTaskRename(LPCSTR id)
{
	if (m_QuestDirty) CommitQuest();
	m_TaskRenameFrom = id ? id : "";
	m_TaskRenameTo = m_TaskRenameFrom;
	m_TaskRenameError.clear();
	m_OpenTaskRename = true;
}

void NqInspector::DrawTaskRename()
{
	if (m_OpenTaskRename)
	{
		ImGui::OpenPopup("Rename Task###nq_task_rename");
		m_OpenTaskRename = false;
	}
	if (!ImGui::BeginPopupModal("Rename Task###nq_task_rename", 0,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;

	ImGui::Text("Rename task '%s'", m_TaskRenameFrom.c_str());
	ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 12.f);
	InputStr("new id", m_TaskRenameTo);
	ImGui::Spacing();
	ImGui::TextColored(ImVec4(1.f, 0.72f, 0.30f, 1.f), "This changes the task's runtime and save identity.");
	ImGui::TextWrapped("Existing saves and running quest state recorded under '%s' are not migrated.",
		m_TaskRenameFrom.c_str());
	if (!m_TaskRenameError.empty())
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", m_TaskRenameError.c_str());
	ImGui::Separator();
	const bool appearing = ImGui::IsWindowAppearing();
	const bool cancel = ImGui::Button("Cancel");
	if (appearing) ImGui::SetItemDefaultFocus();
	if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		m_TaskRenameFrom.clear();
		ImGui::CloseCurrentPopup();
	}
	else
	{
		ImGui::SameLine();
		if (ImGui::Button("Rename task"))
		{
			const xr_string to = NqUtil::Trim(m_TaskRenameTo);
			xr_string error;
			int updated = 0;
			if (m_Doc->RenameTask(m_TaskRenameFrom.c_str(), to.c_str(), error, updated))
			{
				m_QuestRev = u32(-1);
				m_TaskRenameFrom.clear();
				ImGui::CloseCurrentPopup();
			}
			else m_TaskRenameError = error;
		}
	}
	ImGui::EndPopup();
}

void NqInspector::BeginTaskReferences(LPCSTR id)
{
	if (m_QuestDirty) CommitQuest();
	m_TaskReferencesId = id ? id : "";
	m_TaskReferences.clear();
	m_TaskReferenceDiagnostics.clear();
	m_TaskReferencesError.clear();
	m_TaskReferencesComplete = false;
	m_TaskReferencesGeneration = 0;

	NqProjectIndex::SSnapshot snapshot;
	xr_string error;
	if (!NqProjectIndex::Snapshot(snapshot, error)) m_TaskReferencesError = error;
	else
	{
		NqReferences::SResult result;
		if (!NqReferences::Find(snapshot, "task_id", m_TaskReferencesId.c_str(), m_Doc->path.c_str(), result, error))
			m_TaskReferencesError = error;
		else
		{
			m_TaskReferences = result.references;
			m_TaskReferenceDiagnostics = result.diagnostics;
			m_TaskReferencesComplete = result.complete;
			m_TaskReferencesGeneration = result.generation;
		}
	}
	m_OpenTaskReferences = true;
}

void NqInspector::DrawTaskReferences()
{
	if (m_OpenTaskReferences)
	{
		ImGui::OpenPopup("Task References###nq_task_references");
		m_OpenTaskReferences = false;
	}
	if (!ImGui::BeginPopupModal("Task References###nq_task_references", 0,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;

	ImGui::Text("task_id '%s': %d reference(s)", m_TaskReferencesId.c_str(), static_cast<int>(m_TaskReferences.size()));
	if (m_TaskReferencesGeneration)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("index generation %u", m_TaskReferencesGeneration);
	}
	if (!m_TaskReferencesComplete)
		ImGui::TextColored(ImVec4(1.f, 0.72f, 0.30f, 1.f), "Incomplete: some content could not be proven safe.");
	if (!m_TaskReferencesError.empty())
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", m_TaskReferencesError.c_str());

	ImGui::BeginChild("##task_refs", ImVec2(ImGui::GetFrameHeight() * 32.f,
		ImGui::GetTextLineHeightWithSpacing() * 12.f), true, ImGuiWindowFlags_HorizontalScrollbar);
	for (u32 i = 0; i < m_TaskReferences.size(); ++i)
	{
		const NqReferences::SReference& reference = m_TaskReferences[i];
		const xr_string label = NqUtil::Format("%s [%s] %s.%s", reference.node.c_str(), reference.slot.c_str(),
			reference.kind.c_str(), reference.param.c_str());
		if (ImGui::Selectable(label.c_str()))
		{
			ImGui::CloseCurrentPopup();
			m_Doc->selection.clear();
			m_Doc->selection.push_back(reference.node);
			const size_t slash = reference.slot.find('/');
			const xr_string base = slash == xr_string::npos ? reference.slot : reference.slot.substr(0, slash);
			if (base.find("enter:") == 0 || base.find("exit:") == 0) m_Doc->sel_slot = base;
			else m_Doc->sel_slot.clear();
		}
	}
	if (m_TaskReferences.empty() && m_TaskReferencesError.empty()) ImGui::TextDisabled("no typed references");
	for (u32 i = 0; i < m_TaskReferenceDiagnostics.size(); ++i)
	{
		const xr_string text = m_TaskReferenceDiagnostics[i].Text();
		ImGui::TextColored(ImVec4(1.f, 0.72f, 0.30f, 1.f), "%s", text.c_str());
	}
	ImGui::EndChild();
	if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

//------------------------------------------------------------------------------
// variables
//------------------------------------------------------------------------------
void NqInspector::DrawHSplitter(LPCSTR id, float& frac, float total)
{
	const ImGuiStyle& st = ImGui::GetStyle();
	const float grab = ImGui::GetFrameHeight() * 0.35f + st.ItemSpacing.y;
	const ImVec2 top = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, ImVec2(-1.f, grab));
	const bool active = ImGui::IsItemActive();
	const bool hovered = ImGui::IsItemHovered();

	if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frac = 0.30f;
	// the pane grows upwards, so dragging down takes height away from it
	else if (active && total > 1.f) frac -= ImGui::GetIO().MouseDelta.y / total;
	if (active || hovered)
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
		if (active)
		{
			frac = _max(0.10f, _min(frac, 0.80f));
			if (CLevelPreferences* prefs = dynamic_cast<CLevelPreferences*>(EPrefs))
				prefs->QuestVarsSplit = u32(frac * 1000.f + 0.5f);
		}
	}

	const ImU32 col = ImGui::GetColorU32(active		? ImGuiCol_SeparatorActive
										: hovered	? ImGuiCol_SeparatorHovered
													: ImGuiCol_Separator);
	const float y = top.y + grab * 0.5f;
	const float half = (active || hovered) ? grab * 0.5f : 1.f;
	ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(top.x, y - half),
		ImVec2(top.x + ImGui::GetItemRectSize().x, y + half), col);
	if (hovered && !active) ImGui::SetTooltip("Drag to resize, double-click to reset");
}

LPCSTR NqInspector::TypeName(SNqValue::EType t)
{
	switch (t)
	{
	case SNqValue::tBool:	return "bool";
	case SNqValue::tNumber:	return "number";
	case SNqValue::tString:	return "string";
	default:				return "?";
	}
}

const SNqVar* NqInspector::FindVar(LPCSTR name) const
{
	if (!name || !name[0]) return 0;
	for (u32 i = 0; i < m_Quest.vars.size(); ++i)
		if (m_Quest.vars[i].name == name) return &m_Quest.vars[i];
	return 0;
}

bool NqInspector::DrawTypedValue(LPCSTR label, SNqValue& v, SNqValue::EType as, float reserve)
{
	switch (as)
	{
	case SNqValue::tBool:
	{
		// a dropdown, not a string: "true" is the only spelling the runtime reads,
		// and typing it by hand is how a condition silently stops matching
		int cur = (v.IsBool() && v.b) ? 1 : 0;
		static const char* items[] = { "false", "true" };
		ImGui::SetNextItemWidth(-(LabelRoom(label) + reserve + ImGui::CalcTextSize("false").x
			+ ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.f));
		if (ImGui::Combo(label, &cur, items, 2)) { v = SNqValue::Bool(cur != 0); return true; }
		return false;
	}
	case SNqValue::tNumber:
	{
		double d = v.IsNumber() ? v.n : 0.0;
		float f = float(d);
		const float steppers = (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x) * 2.f;
		ImGui::SetNextItemWidth(-(LabelRoom(label) + reserve + steppers));
		// %g, not the stock %.3f: a counter reads as 0, not 0.000, and a value that
		// really is fractional still shows its digits
		if (ImGui::InputFloat(label, &f, 0.f, 0.f, "%g")) { v = SNqValue::Number(double(f)); return true; }
		return false;
	}
	default:
	{
		xr_string s = v.IsString() ? v.s : "";
		if (InputStr(label, s, false, 0.f, reserve)) { v = SNqValue::String(s); return true; }
		return false;
	}
	}
}

bool NqInspector::DrawVarNameCombo(LPCSTR label, xr_string& name)
{
	// The list is the quest's own declarations, so an action cannot point at a
	// variable that does not exist - which used to be a plain typed string.
	xr_vector<xr_string> names;
	for (u32 i = 0; i < m_Quest.vars.size(); ++i) names.push_back(m_Quest.vars[i].name);
	// a name from an older asset that is no longer declared stays selectable, or
	// switching nodes would quietly rewrite it
	if (!name.empty() && !FindVar(name.c_str())) names.push_back(name);
	if (names.empty())
	{
		ImGui::TextDisabled("no variables declared - add one in the Variables pane");
		return false;
	}
	return ComboStr(label, name, names, "(unset)");
}

bool NqInspector::RenameVar(LPCSTR from, LPCSTR to)
{
	if (!from || !to || !to[0] || 0 == xr_strcmp(from, to)) return false;
	if (FindVar(to)) return false;						// name already taken
	bool touched = false;
	for (u32 i = 0; i < m_Quest.vars.size(); ++i)
		if (m_Quest.vars[i].name == from) { m_Quest.vars[i].name = to; touched = true; }
	if (!touched) return false;

	// every reference follows: the document is edited directly because the staged
	// node copy only holds the node the author is looking at
	xr_string old_name = from, new_name = to;
	m_Doc->Edit([&](SNqQuest& q)
	{
		for (u32 i = 0; i < q.vars.size(); ++i)
			if (q.vars[i].name == old_name) q.vars[i].name = new_name;
		for (u32 n = 0; n < q.nodes.size(); ++n)
		{
			SNqNode& node = q.nodes[n];
			for (int pass = 0; pass < 2; ++pass)
			{
				xr_vector<SNqAction>& v = node.Slot(pass == 0 ? "enter" : "exit");
				for (u32 a = 0; a < v.size(); ++a)
				{
					if (v[a].kind == "var.set" || v[a].kind == "var.add")
					{
						SNqValue* nm = v[a].params.Get("name");
						if (nm && nm->IsString() && nm->s == old_name) *nm = SNqValue::String(new_name);
					}
					// an action can carry a nested cond list of its own
					RenameVarInValue(v[a].params, old_name.c_str(), new_name.c_str());
				}
			}
			RenameVarInConds(node.cond, old_name.c_str(), new_name.c_str());
		}
	});
	// the staged copy was filled before the rename; take the document's word for it
	m_NodeRev = u32(-1);
	return true;
}

void NqInspector::DrawVarsSection()
{
	ImGui::TextDisabled("Variables");
	ImGui::SameLine();
	ImGui::TextDisabled("  %d", int(m_Quest.vars.size()));
	ImGui::Separator();

	bool ch = false;
	ImGui::PushID("qvars");
	for (u32 i = 0; i < m_Quest.vars.size(); ++i)
	{
		SNqVar& var = m_Quest.vars[i];
		ImGui::PushID((int)i);

		// name: committed when the field is left, so a half-typed name does not
		// rewrite every reference in the graph on the way through
		char namebuf[64];
		strncpy_s(namebuf, sizeof(namebuf), var.name.c_str(), _TRUNCATE);
		ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 6.f);
		if (ImGui::InputText("##name", namebuf, sizeof(namebuf), ImGuiInputTextFlags_EnterReturnsTrue)
			|| ImGui::IsItemDeactivatedAfterEdit())
		{
			xr_string want = NqUtil::Trim(namebuf);
			if (!want.empty() && want != var.name)
			{
				if (RenameVar(var.name.c_str(), want.c_str())) ch = true;
				else Msg("! [nq] variable '%s' already exists", want.c_str());
			}
		}

		ImGui::SameLine();
		// type: the declared default is what gives a variable its type, so changing
		// the type here is what converts the default
		static const char* types[] = { "bool", "number", "string" };
		int cur = var.value.IsBool() ? 0 : var.value.IsNumber() ? 1 : 2;
		const int was = cur;
		ImGui::SetNextItemWidth(ImGui::CalcTextSize("number").x + ImGui::GetFrameHeight()
			+ ImGui::GetStyle().FramePadding.x * 2.f);
		if (ImGui::Combo("##type", &cur, types, 3) && cur != was)
		{
			switch (cur)
			{
			case 0:  var.value = SNqValue::Bool(var.value.IsNumber() ? var.value.n != 0.0 : false); break;
			case 1:  var.value = SNqValue::Number(var.value.IsBool() ? (var.value.b ? 1.0 : 0.0) : 0.0); break;
			default: var.value = SNqValue::String(var.value.IsBool() ? (var.value.b ? "true" : "false")
						: var.value.IsNumber() ? NqUtil::Format("%g", var.value.n).c_str() : ""); break;
			}
			ch = true;
		}

		ImGui::SameLine();
		ch |= DrawTypedValue("##val", var.value, var.value.type, ButtonRoom("x"));
		ImGui::SameLine();
		if (ImGui::SmallButton("x")) { m_Quest.vars.erase(m_Quest.vars.begin() + i); ch = true; ImGui::PopID(); break; }
		ImGui::PopID();
	}

	if (ImGui::SmallButton("+ variable"))
	{
		SNqVar v;
		v.name = NqUtil::Format("var%d", int(m_Quest.vars.size() + 1));
		v.value = SNqValue::Bool(false);
		m_Quest.vars.push_back(v);
		ch = true;
	}
	ImGui::PopID();

	if (m_Quest.vars.empty())
		ImGui::TextDisabled("the value declared here is both the default and the type");

	if (ch) m_QuestDirty = true;
}

//------------------------------------------------------------------------------
// node
//------------------------------------------------------------------------------
void NqInspector::DrawNodeSection()
{
	const NqCatalog::SKind* k = NqCatalog::Find(m_Node.kind.c_str());
	bool trig = NqText::IsTrigger(m_Node.kind.c_str());
	ImGui::TextDisabled(trig ? "Trigger" : "Node");
	ImGui::SameLine();
	ImGui::TextDisabled("  %s", m_Node.kind.c_str());
	ImGui::Separator();

	// id: renamed through the document so references follow
	{
		static char idbuf[128];
		strncpy_s(idbuf, sizeof(idbuf), m_Node.id.c_str(), _TRUNCATE);
		if (m_FocusRename) { ImGui::SetKeyboardFocusHere(); m_FocusRename = false; }
		if (ImGui::InputText("id", idbuf, sizeof(idbuf), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
		{
			xr_string nid = NqUtil::Trim(idbuf);
			if (nid != m_Node.id && !nid.empty())
			{
				CommitNode();
				xr_string err;
				if (m_Doc->RenameNode(m_Node.id.c_str(), nid.c_str(), err))
				{
					m_Doc->selection.clear(); m_Doc->selection.push_back(nid);
					m_NodeId = nid; m_Node.id = nid; m_NodeRev = m_Doc->revision;
				}
				else if (!err.empty()) Msg("! [nq] rename: %s", err.c_str());
			}
		}
	}

	bool ch = false;
	// kind
	{
		xr_string kind = m_Node.kind;
		if (DrawKindCombo("kind", trig ? NqCatalog::useTrigger : NqCatalog::useMain, kind) && kind != m_Node.kind)
		{
			m_Node.kind = kind;
			m_Node.params = SNqValue::Table();
			m_Node.once_set = false;
			k = NqCatalog::Find(kind.c_str());
			// par. 13.9 resets the parameters, not the wiring: an edge on a pin the
			// new kind still declares stays (next -> next, done -> done). Case pins
			// go with the cases the parameter reset just dropped.
			xr_vector<SNqPin> keep;
			for (u32 p = 0; p < m_Node.out.size(); ++p)
				if (k && k->HasPin(m_Node.out[p].first.c_str())) keep.push_back(m_Node.out[p]);
			m_Node.out = keep;
			ch = true;
		}
	}
	// once
	if (!trig && k)
	{
		bool once = m_Node.once_set ? m_Node.once : k->once_default;
		// toggling back to what the kind already does drops the override, same as
		// the reset button - otherwise it stays declared and the button stays lit
		if (ImGui::Checkbox("once", &once))
		{
			m_Node.once = once;
			m_Node.once_set = (once != k->once_default);
			ch = true;
		}
		if (m_Node.once_set) { ImGui::SameLine(); if (ImGui::SmallButton("default##once")) { m_Node.once_set = false; m_Node.once = k->once_default; ch = true; } }
	}

	if (k)
	{
		if (!k->desc.empty()) ImGui::TextWrapped("%s", k->desc.c_str());
		ImGui::Spacing();
		ch |= DrawParams(k, m_Node.params, "np");
	}
	else
		ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "unknown kind - not in the catalog");

	// conditions (all main/trigger kinds may carry them; the validator says where they matter)
	if (ImGui::CollapsingHeader("Conditions", ImGuiTreeNodeFlags_DefaultOpen))
		ch |= DrawCondList(m_Node.cond, "nc", 0);

	// comment
	ch |= InputStr("comment", m_Node.comment);

	// pins summary
	if (!m_Node.out.empty())
	{
		ImGui::Separator();
		ImGui::TextDisabled("Outputs");
		for (u32 p = 0; p < m_Node.out.size(); ++p)
		{
			xr_string tg;
			for (u32 t = 0; t < m_Node.out[p].second.size(); ++t) { if (t) tg += ", "; tg += m_Node.out[p].second[t]; }
			ImGui::BulletText("%s -> %s", m_Node.out[p].first.c_str(), tg.c_str());
		}
	}

	// actions summary with selection
	ImGui::Separator();
	for (int pass = 0; pass < 2; ++pass)
	{
		LPCSTR slot = pass == 0 ? "enter" : "exit";
		xr_vector<SNqAction>& v = m_Node.Slot(slot);
		ImGui::TextDisabled(pass == 0 ? "on_enter" : "on_exit");
		for (u32 i = 0; i < v.size(); ++i)
		{
			const NqCatalog::SKind* ak = NqCatalog::Find(v[i].kind.c_str());
			xr_string sel = xr_string(slot) + ":" + NqUtil::Format("%d", (int)i);
			xr_string label = NqUtil::Format("%d. %s##%s%d", (int)i + 1, ak ? ak->title.c_str() : v[i].kind.c_str(), slot, (int)i);
			if (ImGui::Selectable(label.c_str(), m_Doc->sel_slot == sel)) m_Doc->sel_slot = sel;
		}
	}

	// node problems
	bool any = false;
	for (u32 i = 0; i < m_Doc->problems.size(); ++i)
		if (m_Doc->problems[i].node_id == m_NodeId)
		{
			if (!any) { ImGui::Separator(); any = true; }
			// these sentences name a parameter and say what to do about it - cut
			// them off at the panel edge and they stop being actionable
			ImGui::PushID((int)i);
			const xr_string ptext = m_Doc->problems[i].Text();
			ImGui::PushStyleColor(ImGuiCol_Text, m_Doc->problems[i].IsError() ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(1, 0.85f, 0.4f, 1));
			ImGui::TextWrapped("%s", ptext.c_str());
			ImGui::PopStyleColor();
			// same right-click copy as the problems strip of the tab
			if (ImGui::BeginPopupContextItem("##nq_prob_ctx"))
			{
				if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(ptext.c_str());
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}

	if (ch) m_NodeDirty = true;
}

//------------------------------------------------------------------------------
// action
//------------------------------------------------------------------------------
void NqInspector::DrawActionSection()
{
	xr_string slot; int index;
	SplitSlot(m_Doc->sel_slot, slot, index);
	// SNqNode::Slot answers on_enter for every name it does not know, so an
	// unexpected slot would silently edit the wrong list
	if (!m_Node.SlotC(slot.c_str())) { ImGui::TextDisabled("no action selected"); m_Doc->sel_slot.clear(); return; }
	xr_vector<SNqAction>& v = m_Node.Slot(slot.c_str());
	if (index < 0 || index >= (int)v.size()) { ImGui::TextDisabled("no action selected"); m_Doc->sel_slot.clear(); return; }
	SNqAction& a = v[index];
	ImGui::TextDisabled("Action  %s #%d", slot.c_str(), index + 1);
	// a flat 90px reservation is not enough for these three at any DPI, and what
	// does not fit is drawn past the edge where it cannot be clicked
	const float tools = ButtonRoom("up") + ButtonRoom("down") + ButtonRoom("remove");
	const float row = ImGui::GetContentRegionAvail().x;
	ImGui::SameLine(row > tools ? row - tools : 0.f);
	bool ch = false;
	if (ImGui::SmallButton("up") && index > 0)   { std::swap(v[index], v[index - 1]); m_Doc->sel_slot = slot + ":" + NqUtil::Format("%d", index - 1); ch = true; }
	ImGui::SameLine();
	if (ImGui::SmallButton("down") && index + 1 < (int)v.size()) { std::swap(v[index], v[index + 1]); m_Doc->sel_slot = slot + ":" + NqUtil::Format("%d", index + 1); ch = true; }
	ImGui::SameLine();
	if (ImGui::SmallButton("remove")) { v.erase(v.begin() + index); m_Doc->sel_slot.clear(); m_NodeDirty = true; return; }
	ImGui::Separator();
	if (ch) { m_NodeDirty = true; return; }

	if (m_FocusAction) { ImGui::SetKeyboardFocusHere(); m_FocusAction = false; }
	xr_string kind = a.kind;
	if (DrawKindCombo("kind##a", NqCatalog::useExtra, kind) && kind != a.kind) { a.kind = kind; a.params = SNqValue::Table(); ch = true; }
	const NqCatalog::SKind* k = NqCatalog::Find(a.kind.c_str());
	if (k)
	{
		if (!k->desc.empty()) ImGui::TextWrapped("%s", k->desc.c_str());
		ch |= DrawParams(k, a.params, "ap");
	}
	else ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "unknown kind");
	if (ch) m_NodeDirty = true;
}

//------------------------------------------------------------------------------
// generic editors
//------------------------------------------------------------------------------
bool NqInspector::DrawKindCombo(LPCSTR label, u32 use_mask, xr_string& kind)
{
	bool changed = false;
	const NqCatalog::SKind* cur = NqCatalog::Find(kind.c_str());
	xr_string preview = cur ? cur->title : kind;
	if (ImGui::BeginCombo(label, preview.c_str()))
	{
		xr_vector<const NqCatalog::SKind*> kinds;
		NqCatalog::KindsFor(use_mask, kinds);
		xr_string group;
		for (u32 i = 0; i < kinds.size(); ++i)
		{
			if (kinds[i]->group != group) { group = kinds[i]->group; ImGui::TextDisabled("%s", group.c_str()); }
			xr_string item = kinds[i]->title + "##" + kinds[i]->id;
			if (ImGui::Selectable(item.c_str(), kinds[i]->id == kind)) { kind = kinds[i]->id; changed = true; }
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", kinds[i]->id.c_str(), kinds[i]->desc.c_str());
		}
		ImGui::EndCombo();
	}
	return changed;
}

bool NqInspector::DrawParams(const NqCatalog::SKind* k, SNqValue& params, LPCSTR id_prefix)
{
	bool ch = false;
	if (!params.IsTable()) params = SNqValue::Table();
	ImGui::PushID(id_prefix);
	for (u32 i = 0; i < k->params.size(); ++i)
	{
		ImGui::PushID((int)i);
		ch |= DrawParam(k->params[i], params, id_prefix, k->id.c_str());
		ImGui::PopID();
	}
	// parameters present in the file but unknown to the catalog stay editable as raw Lua
	for (u32 i = 0; i < params.keys.size(); ++i)
	{
		if (k->Param(params.keys[i].c_str())) continue;
		ImGui::PushID(1000 + (int)i);
		xr_string label = params.keys[i] + " (unknown)";
		ch |= DrawRaw(label.c_str(), params.vals[i]);
		ImGui::PopID();
	}
	ImGui::PopID();
	return ch;
}

bool NqInspector::DrawParam(const NqCatalog::SParam& p, SNqValue& params, LPCSTR, LPCSTR kind)
{
	SNqValue* slot = params.Get(p.name.c_str());
	SNqValue v = slot ? *slot : SNqValue::Nil();
	xr_string label = p.name;
	if (p.required) label += " *";
	// a parameter another form has ruled out stays visible but dead: hiding it would
	// make the alternative forms of the kind impossible to discover
	const bool ruled_out = NqCatalog::FormRuledOut(kind, p.name.c_str(), params);
	if (ruled_out) ImGui::BeginDisabled();
	SNqValue* keep = m_ParamsCtx;
	m_ParamsCtx = &params;
	bool ch = DrawTyped(p.type.c_str(), &p, label.c_str(), v);
	m_ParamsCtx = keep;
	if (ruled_out) ImGui::EndDisabled();
	if (ImGui::IsItemHovered())
	{
		xr_string tip = p.type;
		if (p.has_default) tip += " (default " + p.def + ")";
		if (ruled_out) tip += "\nnot used: another form of " + xr_string(kind) + " is set";
		ImGui::SetTooltip("%s", tip.c_str());
	}
	if (ch)
	{
		// Typing the catalog default back is the same statement as not writing the
		// parameter at all, so it stops being written: otherwise the asset carries
		// a redundant value and the "default" button - which means "there is an
		// explicit value here" - never goes away.
		if (v.IsNil() || EqualsDefault(p, v)) params.Erase(p.name.c_str());
		else params.Set(p.name.c_str(), v);
	}
	return ch;
}

bool NqInspector::DrawTyped(LPCSTR type, const NqCatalog::SParam* p, LPCSTR label, SNqValue& v)
{
	xr_string t = type;
	if (t == "text")				return DrawText(label, v, true);
	if (t == "lua")					return DrawLua(label, v);
	if (t == "bool")
	{
		bool set = v.IsBool();
		bool b = set ? v.b : (p && p->has_default && p->def == "true");
		bool ch = false;
		if (ImGui::Checkbox(label, &b)) { v = SNqValue::Bool(b); ch = true; }
		if (set) { ImGui::SameLine(); if (ImGui::SmallButton("default")) { v = SNqValue::Nil(); ch = true; } }
		return ch;
	}
	if (t == "int" || t == "float")
	{
		bool is_int = t == "int";
		double d = v.IsNumber() ? v.n : (p && p->has_default ? atof(p->def.c_str()) : 0.0);
		bool ch = false;
		// InputInt/InputFloat carry their own -/+ steppers, so the row must keep
		// room for the label, the steppers and the reset button that follows
		const float steppers = (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x) * 2.f;
		ImGui::SetNextItemWidth(-(LabelRoom(label) + steppers + (v.IsNil() ? 0.f : ButtonRoom("default"))));
		if (is_int) { int i = (int)d; if (ImGui::InputInt(label, &i)) { d = i; ch = true; } }
		else { float f = (float)d; if (ImGui::InputFloat(label, &f)) { d = f; ch = true; } }
		if (ch)
		{
			if (p && p->has_min && d < p->min) d = p->min;
			if (p && p->has_max && d > p->max) d = p->max;
			v = SNqValue::Number(d);
		}
		if (!v.IsNil()) { ImGui::SameLine(); if (ImGui::SmallButton("default")) { v = SNqValue::Nil(); ch = true; } }
		return ch;
	}
	if (t == "enum" || t == "relation")
	{
		xr_vector<xr_string> items = (t == "relation") ? Items("enemy", "neutral", "friend") : (p ? p->enums : xr_vector<xr_string>());
		xr_string cur = v.IsString() ? v.s : "";
		xr_string def = p && p->has_default ? "(default " + p->def + ")" : "(unset)";
		if (ComboStr(label, cur, items, def.c_str())) { v = cur.empty() ? SNqValue::Nil() : SNqValue::String(cur); return true; }
		return false;
	}
	if (t == "duration")			return DrawDuration(label, v);
	if (t == "npc_ref")				return DrawNpcRef(label, v, false, false);
	if (t == "target_ref")			return DrawNpcRef(label, v, true, false);
	if (t == "object_ref" || t == "squad_ref")	return DrawObjectRef(label, v);
	if (t == "objective_id")		return DrawObjectiveId(label, v);
	if (t == "kill_target")			return DrawNpcRef(label, v, false, true);
	if (t == "place")				return DrawPlace(label, v);
	if (t == "spawn_spec")			return DrawSpawnSpec(label, v);
	if (t == "cases_cond")			return DrawCases(label, v, false);
	if (t == "cases_weight")		return DrawCases(label, v, true);
	if (t == "cond_list")			return DrawCondValue(label, v, 1);
	if (t == "var_name")
	{
		// a dropdown of what the quest actually declares, not a typed-in name
		xr_string s = v.AsString("");
		if (DrawVarNameCombo(label, s)) { v = s.empty() ? SNqValue::Nil() : SNqValue::String(s); return true; }
		return false;
	}
	if (t == "value")
	{
		// `value` always sits next to the `name` it is assigned to or compared
		// with, so the variable's declared type decides the widget: a bool gets a
		// dropdown, a number a numeric field, and nobody types 5 into a string
		if (m_ParamsCtx)
		{
			const SNqValue* nm = m_ParamsCtx->Get("name");
			if (nm && nm->IsString())
				if (const SNqVar* var = FindVar(nm->s.c_str()))
					return DrawTypedValue(label, v, var->value.type);
		}
		return DrawScalarValue(label, v);
	}
	if (t == "count_or_all")
	{
		xr_string s = v.IsNumber() ? NqUtil::Format("%d", (int)v.n) : v.AsString("");
		if (InputStr(label, s))
		{
			s = NqUtil::Trim(s);
			if (s.empty()) v = SNqValue::Nil();
			else if (s == "all") v = SNqValue::String("all");
			else v = SNqValue::Number(atof(s.c_str()));
			return true;
		}
		return false;
	}
	// strings with (or without) a picker
	xr_string s = v.AsString("");
	if (DrawPicked(label, type, s)) { v = s.empty() ? SNqValue::Nil() : SNqValue::String(s); return true; }
	return false;
}

bool NqInspector::PickerPopup(LPCSTR popup, LPCSTR type, xr_string& out)
{
	if (!ImGui::IsPopupOpen(popup)) return false;

	// The rows are gathered BEFORE the window opens. A popup auto-sizes to its
	// content, but the list lives in a child with an explicit size, so the width
	// has to be measured here - otherwise "bar_visitors_cardan_tech_squad - Гро..."
	// is all the author ever gets to read.
	const xr_string t = type;
	const bool doc_scoped = (t == "task_id" || t == "var_name" || t == "ref_name" || t == "node_id" || t == "quest_id");
	xr_vector<xr_string> labels, values, extras;
	LPCSTR note = 0;

	if (doc_scoped)
	{
		xr_vector<xr_string> ids;
		const SNqQuest& q = m_Doc->quest;
		if (t == "task_id")	for (u32 i = 0; i < q.tasks.size(); ++i) ids.push_back(q.tasks[i].id);
		if (t == "var_name") for (u32 i = 0; i < q.vars.size(); ++i) ids.push_back(q.vars[i].name);
		if (t == "node_id")	for (u32 i = 0; i < q.nodes.size(); ++i) ids.push_back(q.nodes[i].id);
		if (t == "ref_name") NqValidate::DeclaredRefs(q, ids);
		if (t == "quest_id")
		{
			if (m_ProjectQuestIdsSerial != NqProjectIndex::InvalidationSerial()) RefreshProjectQuestIds();
			ids = m_ProjectQuestIds;
		}
		for (u32 i = 0; i < ids.size(); ++i)
		{
			if (m_Search[0] && !ContainsNoCaseAscii(ids[i], m_Search)) continue;
			labels.push_back(ids[i]);
			values.push_back(ids[i]);
			extras.push_back(xr_string());
		}
		if (labels.empty()) note = ids.empty() ? "nothing declared in this quest" : "nothing matches";
	}
	else
	{
		const NqPickers::EType pt = NqPickers::TypeFromName(type);
		if (pt == NqPickers::tCount)			note = "no index for this parameter";
		else if (!NqPickers::Available())		note = "link a game to get suggestions";
		else
		{
			xr_vector<const NqPickers::SEntry*> found;
			NqPickers::Search(pt, m_Search, 200, found);
			for (u32 i = 0; i < found.size(); ++i)
			{
				xr_string label = found[i]->id;
				if (!found[i]->name.empty()) label += "  -  " + found[i]->name;
				labels.push_back(label);
				values.push_back(found[i]->id);
				// who they are and how many, where a smart stands, which level a
				// restrictor is on - the index knows, and a list of bare ids does not
				extras.push_back(found[i]->extra);
			}
			if (labels.empty()) note = "nothing matches";
		}
	}

	const ImGuiStyle& st = ImGui::GetStyle();
	const float em = ImGui::GetFrameHeight();
	float w = ImGui::CalcTextSize("nothing declared in this quest").x;
	for (u32 i = 0; i < labels.size(); ++i)
	{
		float row = ImGui::CalcTextSize(labels[i].c_str()).x;
		if (i < extras.size() && !extras[i].empty())
			row += ImGui::CalcTextSize("  ").x + ImGui::CalcTextSize(extras[i].c_str()).x;
		w = _max(w, row);
	}
	w += st.WindowPadding.x * 2.f + st.FramePadding.x * 2.f + st.ScrollbarSize;
	// wide enough to be worth opening, capped so a stray long id cannot span the
	// screen; anything past the cap stays reachable through the scrollbar
	w = _min(_max(w, em * 16.f), em * 34.f);
	ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.f), ImVec2(w, FLT_MAX));

	bool picked = false;
	if (!ImGui::BeginPopup(popup)) return false;
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputTextWithHint("##search", "search", m_Search, sizeof(m_Search));
	ImGui::Separator();

	const float rows = _min(labels.empty() ? 1.f : float(labels.size()), 14.f);
	ImGui::BeginChild("##list", ImVec2(-1.f, ImGui::GetTextLineHeightWithSpacing() * rows),
		false, ImGuiWindowFlags_HorizontalScrollbar);
	for (u32 i = 0; i < labels.size(); ++i)
	{
		if (ImGui::Selectable(labels[i].c_str())) { out = values[i]; picked = true; ImGui::CloseCurrentPopup(); }
		if (i < extras.size() && !extras[i].empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", extras[i].c_str());
		}
	}
	if (note) ImGui::TextDisabled("%s", note);
	ImGui::EndChild();

	ImGui::EndPopup();
	return picked;
}

bool NqInspector::DrawPicked(LPCSTR label, LPCSTR type, xr_string& s)
{
	bool ch = false;
	ImGui::PushID(label);
	ch |= InputStr(label, s, false, 0.f, ButtonRoom("..."));
	ImGui::SameLine();
	// the filter is cleared here, not on the appearing frame: the list is gathered
	// before the popup opens, so a leftover query would decide its width
	if (ImGui::SmallButton("..."))
	{
		m_Search[0] = 0;
		if (0 == strcmp(type, "quest_id")) RefreshProjectQuestIds();
		ImGui::OpenPopup("pick");
	}
	if (PickerPopup("pick", type, s)) ch = true;
	ImGui::PopID();
	return ch;
}

void NqInspector::RefreshProjectQuestIds()
{
	m_ProjectQuestIds.clear();
	NqDocs::OtherQuestIds(m_Doc->path.c_str(), m_ProjectQuestIds);
	m_ProjectQuestIds.insert(m_ProjectQuestIds.begin(), m_Doc->quest.id);
	m_ProjectQuestIdsSerial = NqProjectIndex::InvalidationSerial();
}

bool NqInspector::DrawText(LPCSTR label, SNqValue& v, bool multiline)
{
	bool ch = false;
	ImGui::PushID(label);
	if (v.IsTable())
	{
		xr_string lt;
		ImGui::TextDisabled("%s (per language)", Shown(label, lt));
		for (u32 i = 0; i < v.keys.size(); ++i)
		{
			xr_string s = v.vals[i].AsString("");
			ImGui::PushID((int)i);
			if (InputStr(v.keys[i].c_str(), s, multiline, FitRows(s, 3, 12))) { v.vals[i] = SNqValue::String(s); ch = true; }
			ImGui::PopID();
		}
		if (!v.Has("eng") && ImGui::SmallButton("+ eng")) { v.Set("eng", SNqValue::String("")); ch = true; }
		ImGui::SameLine();
		if (ImGui::SmallButton("single")) { xr_string keep = NqText::Preview(v); v = SNqValue::String(keep); ch = true; }
	}
	else
	{
		xr_string s = v.AsString("");
		if (InputStr(label, s, multiline, FitRows(s, 4, 16))) { v = SNqValue::String(s); ch = true; }
		if (ImGui::SmallButton("per language")) { SNqValue t = SNqValue::Table(); t.Set("rus", SNqValue::String(s)); v = t; ch = true; }
	}
	ImGui::PopID();
	return ch;
}

bool NqInspector::DrawDuration(LPCSTR label, SNqValue& v)
{
	static const xr_vector<xr_string> units = Items("seconds", "game_minutes", "game_hours");
	xr_string unit = ModeOf(v, units);
	double num = unit.empty() ? 0.0 : v.GetNumber(unit.c_str());
	bool ch = false;
	ImGui::PushID(label);
	ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 5.f);
	float f = (float)num;
	if (ImGui::InputFloat("##n", &f)) { num = f; ch = true; }
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ComboRoom(units));
	xr_string u = unit.empty() ? "seconds" : unit;
	if (ComboStr("##u", u, units)) { unit = u; ch = true; }
	ImGui::SameLine();
	xr_string lt;
	ImGui::TextUnformatted(Shown(label, lt));
	if (ch)
	{
		if (unit.empty()) unit = "seconds";
		SNqValue t = SNqValue::Table();
		t.Set(unit.c_str(), SNqValue::Number(num));
		v = t;
	}
	ImGui::PopID();
	return ch;
}

bool NqInspector::DrawNpcRef(LPCSTR label, SNqValue& v, bool target, bool kill)
{
	xr_vector<xr_string> modes = Items("story", "ref", "profile", "community");
	// a target names a spot as well as a creature: the runtime resolves a zone by
	// name and anchors a bare position with a restrictor it creates itself
	if (target) { modes.push_back("smart"); modes.push_back("restrictor"); modes.push_back("pos"); }
	if (kill) modes.push_back("spawn");
	xr_string mode = ModeOf(v, modes);
	if (mode.empty() && target && v.IsTable() && v.Has("pos")) mode = "pos";
	bool ch = false;
	ImGui::PushID(label);
	xr_string lt;
	ImGui::TextUnformatted(Shown(label, lt));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ComboRoom(modes, "(unset)"));
	xr_string m = mode;
	if (ComboStr("##mode", m, modes, "(unset)"))
	{
		ch = true;
		if (m.empty()) v = SNqValue::Nil();
		else
		{
			SNqValue t = SNqValue::Table();
			if (m == "spawn") { SNqValue s = SNqValue::Table(); s.Set("section", SNqValue::String("")); s.Set("smart", SNqValue::String("")); t.Set("spawn", s); }
			else if (m == "pos")
			{
				t.Set("level", SNqValue::String(NqDocs::CurrentLevel()));
				SNqValue p = SNqValue::Table();
				for (int i = 0; i < 3; ++i) p.Push(SNqValue::Number(0));
				t.Set("pos", p);
				t.Set("radius", SNqValue::Number(5));
			}
			else t.Set(m.c_str(), SNqValue::String(""));
			v = t;
		}
		mode = m;
	}
	if (mode == "pos") ch |= DrawPosition(v);
	else if (!mode.empty() && mode != "spawn")
	{
		xr_string s = v.GetString(mode.c_str());
		LPCSTR type = mode == "story" ? "story_id" : mode == "ref" ? "ref_name" : mode.c_str();
		if (DrawPicked("##value", type, s)) { v.Set(mode.c_str(), SNqValue::String(s)); ch = true; }
		if (mode == "community")
		{
			xr_string lvl = v.GetString("level");
			if (DrawPicked("level##community", "level", lvl)) { if (lvl.empty()) v.Erase("level"); else v.Set("level", SNqValue::String(lvl)); ch = true; }
		}
	}
	else if (mode == "spawn")
	{
		SNqValue* sp = v.Get("spawn");
		if (sp && DrawSpawnSpec("spawn", *sp)) ch = true;
	}
	ImGui::PopID();
	return ch;
}

// object_ref / squad_ref: one concrete object, so only the two ways to name one -
// a profile or a community would describe a kind of creature, not a thing.
// The steps of whichever task the sibling "task" parameter names. Typing an id by
// hand would be guessing, and the validator would only tell you afterwards.
bool NqInspector::DrawObjectiveId(LPCSTR label, SNqValue& v)
{
	const SNqValue* owner = m_ParamsCtx ? m_ParamsCtx->Get("task") : 0;
	const SNqTask* task = (owner && owner->IsString()) ? m_Quest.FindTask(owner->s.c_str()) : 0;
	xr_vector<xr_string> ids;
	if (task)
		for (u32 i = 0; i < task->objectives.size(); ++i) ids.push_back(task->objectives[i].id);

	xr_string cur = v.IsString() ? v.s : xr_string();
	bool ch = false;
	ImGui::PushID(label);
	xr_string lt;
	ImGui::TextUnformatted(Shown(label, lt));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1.f);
	if (!task)
	{
		ImGui::TextDisabled(owner && owner->IsString() ? "pick a task that exists first" : "pick the task first");
		ImGui::PopID();
		return false;
	}
	if (ids.empty())
	{
		ImGui::TextDisabled("task '%s' has no objectives yet", task->id.c_str());
		ImGui::PopID();
		return false;
	}
	if (ComboStr("##objective", cur, ids, "(unset)"))
	{
		v = cur.empty() ? SNqValue::Nil() : SNqValue::String(cur);
		ch = true;
	}
	ImGui::PopID();
	return ch;
}

bool NqInspector::DrawObjectRef(LPCSTR label, SNqValue& v)
{
	xr_vector<xr_string> modes = Items("story", "ref");
	xr_string mode = ModeOf(v, modes);
	bool ch = false;
	ImGui::PushID(label);
	xr_string lt;
	ImGui::TextUnformatted(Shown(label, lt));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ComboRoom(modes, "(unset)"));
	xr_string m = mode;
	if (ComboStr("##mode", m, modes, "(unset)"))
	{
		ch = true;
		if (m.empty()) v = SNqValue::Nil();
		else { SNqValue t = SNqValue::Table(); t.Set(m.c_str(), SNqValue::String("")); v = t; }
		mode = m;
	}
	if (!mode.empty())
	{
		xr_string s = v.GetString(mode.c_str());
		if (DrawPicked("##value", mode == "story" ? "story_id" : "ref_name", s))
		{
			v.Set(mode.c_str(), SNqValue::String(s));
			ch = true;
		}
	}
	ImGui::PopID();
	return ch;
}

// One line under a field saying what it is for. Cheaper than a tooltip nobody hovers,
// and these rows are the ones people cannot tell apart.
static void Hint(LPCSTR text)
{
	ImGui::Indent();
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
	ImGui::Unindent();
}

bool NqInspector::DrawSpawnSpec(LPCSTR label, SNqValue& v)
{
	if (!v.IsTable()) v = SNqValue::Table();
	bool ch = false;
	ImGui::PushID(label);
	ImGui::Indent();
	// Two of these rows can both name a restrictor - where they appear and where they
	// are penned in - so each says what it is for rather than what type it takes.
	xr_string s = v.GetString("section");
	if (DrawPicked("section", "squad_section", s)) { v.Set("section", SNqValue::String(s)); ch = true; }
	Hint("which squad to create; its section decides how many and who they are");
	s = v.GetString("smart");
	if (DrawPicked("smart", "smart", s)) { v.Set("smart", SNqValue::String(s)); ch = true; }
	Hint("the smart terrain that owns them - it is what gives them something to do");

	SNqValue* pl = v.Get("place");
	SNqValue place = pl ? *pl : SNqValue::Nil();
	if (DrawPlace("place: where they appear", place))
	{
		if (place.IsNil()) v.Erase("place"); else v.Set("place", place);
		ch = true;
	}
	Hint("empty = on the smart. A zone scatters them across it, a point puts them all on it");

	if (place.IsTable() && place.Has("restrictor"))
	{
		float spread = (float)v.GetNumber("spread", 1.0);
		if (ImGui::SliderFloat("spread##spawn", &spread, 0.05f, 1.f, "%.2f"))
		{
			if (spread >= 0.999f) v.Erase("spread"); else v.Set("spread", SNqValue::Number(spread));
			ch = true;
		}
		Hint("how much of the zone to spawn in: 0.5 keeps them off the edges. The zone itself does not change");
	}

	s = v.GetString("restrictor");
	if (DrawPicked("restrictor: they cannot leave it", "restrictor", s)) { if (s.empty()) v.Erase("restrictor"); else v.Set("restrictor", SNqValue::String(s)); ch = true; }
	Hint("a zone they are penned into. They keep working, they just cannot wander off");
	s = v.GetString("ref");
	if (DrawPicked("ref: name to refer to them later", "ref_name", s)) { if (s.empty()) v.Erase("ref"); else v.Set("ref", SNqValue::String(s)); ch = true; }
	Hint("other nodes point at this squad by that name");
	bool hold = v.GetBool("hold", true);
	if (ImGui::Checkbox("hold", &hold)) { v.Set("hold", SNqValue::Bool(hold)); ch = true; }
	Hint("pins the squad to its smart, so the simulation cannot send it elsewhere");
	ImGui::Unindent();
	ImGui::PopID();
	return ch;
}

bool NqInspector::DrawPlace(LPCSTR label, SNqValue& v)
{
	// "ref" names an object this quest created - the only way to point at something
	// that did not exist when the graph was written
	xr_vector<xr_string> modes = Items("pos", "restrictor", "smart", "ref");
	xr_string mode = ModeOf(v, modes);
	if (mode.empty() && v.IsTable() && v.Has("level")) mode = "pos";
	bool ch = false;
	ImGui::PushID(label);
	xr_string lt;
	ImGui::TextUnformatted(Shown(label, lt));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ComboRoom(modes, "(unset)"));
	xr_string m = mode;
	if (ComboStr("##mode", m, modes, "(unset)"))
	{
		ch = true;
		if (m.empty()) v = SNqValue::Nil();
		else
		{
			SNqValue t = SNqValue::Table();
			if (m == "pos") { t.Set("level", SNqValue::String(NqDocs::CurrentLevel())); SNqValue p = SNqValue::Table(); p.Push(SNqValue::Number(0)); p.Push(SNqValue::Number(0)); p.Push(SNqValue::Number(0)); t.Set("pos", p); t.Set("radius", SNqValue::Number(5)); }
			else t.Set(m.c_str(), SNqValue::String(""));
			v = t;
		}
		mode = m;
	}
	if (mode == "pos") ch |= DrawPosition(v);
	else if (!mode.empty())
	{
		// The field is labelled with the mode: an unnamed box under a combo, next to a
		// second box that says "restrictor", is two blanks and a guess.
		xr_string s = v.GetString(mode.c_str());
		LPCSTR type = (mode == "ref") ? "ref_name" : mode.c_str();
		xr_string field = mode + "##place_value";
		if (DrawPicked(field.c_str(), type, s)) { v.Set(mode.c_str(), SNqValue::String(s)); ch = true; }
	}
	ImGui::PopID();
	return ch;
}

// Shared by every variant that spells a spot out by hand: place{pos} and a task
// target{pos}, which the runtime anchors with a restrictor of its own.
bool NqInspector::DrawPosition(SNqValue& v)
{
	bool ch = false;
	ImGui::Indent();
	xr_string lvl = v.GetString("level");
	if (DrawPicked("level", "level", lvl)) { v.Set("level", SNqValue::String(lvl)); ch = true; }
	SNqValue* pos = v.Get("pos");
	float xyz[3] = { 0, 0, 0 };
	if (pos && pos->IsTable()) for (u32 i = 0; i < 3 && i < pos->arr.size(); ++i) xyz[i] = (float)pos->arr[i].AsNumber();
	if (ImGui::InputFloat3("x y z", xyz))
	{
		SNqValue p = SNqValue::Table();
		for (int i = 0; i < 3; ++i) p.Push(SNqValue::Number(xyz[i]));
		v.Set("pos", p); ch = true;
	}
	// The point is a sphere, not a pixel. Saying so on the label, because "radius"
	// under an x/y/z reads as decoration and the author is left thinking they have to
	// stand exactly on the spot.
	float r = (float)v.GetNumber("radius", 5.0);
	if (ImGui::InputFloat("radius, m", &r)) { v.Set("radius", SNqValue::Number(r)); ch = true; }
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("How close the player has to get, in metres. 5 when not set.");
	if (ImGui::SmallButton("from camera"))
	{
		// the current scene level and the viewport camera position
		SNqValue p = SNqValue::Table();
		p.Push(SNqValue::Number(EDevice->vCameraPosition.x));
		p.Push(SNqValue::Number(EDevice->vCameraPosition.y));
		p.Push(SNqValue::Number(EDevice->vCameraPosition.z));
		v.Set("pos", p);
		xr_string cl = NqDocs::CurrentLevel();
		if (!cl.empty()) v.Set("level", SNqValue::String(cl));
		ch = true;
	}
	ImGui::Unindent();
	return ch;
}

bool NqInspector::DrawCases(LPCSTR label, SNqValue& v, bool weights)
{
	if (!v.IsTable()) v = SNqValue::Table();
	bool ch = false;
	ImGui::PushID(label);
	xr_string lt;
	ImGui::TextDisabled("%s", Shown(label, lt));
	for (u32 i = 0; i < v.arr.size(); ++i)
	{
		SNqValue& c = v.arr[i];
		if (!c.IsTable()) c = SNqValue::Table();
		ImGui::PushID((int)i);
		xr_string name = c.GetString("name");
		ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 6.f);
		if (InputStr("##name", name)) { c.Set("name", SNqValue::String(name)); ch = true; }
		ImGui::SameLine();
		if (weights)
		{
			float w = (float)c.GetNumber("weight", 1.0);
			ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 4.f);
			if (ImGui::InputFloat("weight", &w)) { c.Set("weight", SNqValue::Number(w)); ch = true; }
			ImGui::SameLine();
		}
		if (ImGui::SmallButton("x")) { v.arr.erase(v.arr.begin() + i); ch = true; ImGui::PopID(); break; }
		if (!weights)
		{
			// the case's cond list lives as generic values; edit through typed conds
			SNqValue* cl = c.Get("cond");
			SNqValue tmp = cl ? *cl : SNqValue::Table();
			ImGui::Indent();
			if (DrawCondValue("cond", tmp, 1)) { c.Set("cond", tmp); ch = true; }
			ImGui::Unindent();
		}
		ImGui::PopID();
	}
	if (ImGui::SmallButton("+ case"))
	{
		SNqValue c = SNqValue::Table();
		c.Set("name", SNqValue::String(NqUtil::Format("case%d", int(v.arr.size() + 1))));
		if (weights) c.Set("weight", SNqValue::Number(1));
		else c.Set("cond", SNqValue::Table());
		v.Push(c); ch = true;
	}
	ImGui::PopID();
	return ch;
}

bool NqInspector::DrawCondValue(LPCSTR label, SNqValue& v, int depth)
{
	// generic cond list <-> typed conds round trip
	xr_vector<SNqCond> conds;
	if (v.IsTable())
		for (u32 i = 0; i < v.arr.size(); ++i)
		{
			SNqCond c; xr_string err;
			if (NqLua::CondFromValue(v.arr[i], c, err)) conds.push_back(c);
		}
	if (!DrawCondList(conds, label, depth)) return false;
	SNqValue out = SNqValue::Table();
	for (u32 i = 0; i < conds.size(); ++i) { SNqValue cv; NqLua::CondToValue(conds[i], cv); out.Push(cv); }
	v = out;
	return true;
}

bool NqInspector::DrawCondList(xr_vector<SNqCond>& conds, LPCSTR id_prefix, int depth)
{
	bool ch = false;
	ImGui::PushID(id_prefix);
	for (u32 i = 0; i < conds.size(); ++i)
	{
		SNqCond& c = conds[i];
		ImGui::PushID((int)i);
		if (ImGui::Checkbox("not", &c.negate)) ch = true;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-ButtonRoom("x"));
		xr_string kind = c.kind;
		if (DrawKindCombo("##kind", NqCatalog::useCond, kind) && kind != c.kind) { c.kind = kind; c.params = SNqValue::Table(); ch = true; }
		ImGui::SameLine();
		if (ImGui::SmallButton("x")) { conds.erase(conds.begin() + i); ch = true; ImGui::PopID(); break; }
		const NqCatalog::SKind* k = NqCatalog::Find(c.kind.c_str());
		if (k && !k->params.empty() && depth < 4)
		{
			ImGui::Indent();
			ch |= DrawParams(k, c.params, "cp");
			ImGui::Unindent();
		}
		ImGui::PopID();
	}
	if (ImGui::SmallButton("+ condition"))
	{
		SNqCond c; c.kind = "has_item"; c.params = SNqValue::Table();
		conds.push_back(c); ch = true;
	}
	ImGui::PopID();
	return ch;
}

bool NqInspector::DrawLua(LPCSTR label, SNqValue& v)
{
	bool ch = false;
	ImGui::PushID(label);
	xr_string code = v.AsString("");
	if (InputStr(label, code, true, FitRows(code, 10, 30))) { v = SNqValue::String(code); ch = true; }
	if (ImGui::SmallButton("check syntax"))
	{
		NqLua::SError err;
		m_LuaCheck = NqLua::SyntaxCheck(code.c_str(), err) ? "ok" : NqUtil::Format("%s (%d:%d)", err.message.c_str(), err.line, err.col);
	}
	if (!m_LuaCheck.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", m_LuaCheck.c_str()); }
	ImGui::PopID();
	return ch;
}

bool NqInspector::DrawScalarValue(LPCSTR label, SNqValue& v)
{
	// bool / number / string typed by content: true, false, 12.5, anything else
	xr_string s = v.IsBool() ? (v.b ? "true" : "false") : (v.IsNumber() ? NqUtil::Format("%g", v.n) : v.AsString(""));
	if (!InputStr(label, s)) return false;
	xr_string t = NqUtil::Trim(s);
	if (t == "true") v = SNqValue::Bool(true);
	else if (t == "false") v = SNqValue::Bool(false);
	else
	{
		char* end = 0;
		double d = strtod(t.c_str(), &end);
		if (!t.empty() && end && *end == 0) v = SNqValue::Number(d);
		else v = SNqValue::String(s);
	}
	return true;
}

bool NqInspector::DrawRaw(LPCSTR label, SNqValue& v)
{
	xr_string s = NqLua::WriteValue(v, 0);
	if (!InputStr(label, s)) return false;
	SNqValue nv; NqLua::SError err;
	xr_string chunk = "return " + s;
	if (NqLua::ParseValue(chunk.c_str(), (u32)chunk.size(), "raw", nv, err)) { v = nv; return true; }
	return false;
}
