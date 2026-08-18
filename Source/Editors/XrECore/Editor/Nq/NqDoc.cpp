#include "stdafx.h"
#include "NqDoc.h"
#include "NqLua.h"
#include "NqUtil.h"
#include "NqCatalog.h"
#include "NqPickers.h"
#include "NqLayout.h"
#include "NqReferences.h"
#include "NqProjectIndex.h"
#include "../EditorProject.h"
#include "../UI_ToolsCustom.h"

namespace
{
	const u32 kUndoRing = 200;
}

//==============================================================================
// NqDoc
//==============================================================================
NqDoc::NqDoc()
	: dirty(false), view_cx(0.f), view_cy(0.f), zoom_idx(6), revision(0), m_FileTime(0), m_Partial(false)
{
}

bool NqDoc::FileExists() const			{ return NqUtil::FileExists(path.c_str()); }
bool NqDoc::ModifiedExternally() const	{ return m_FileTime != 0 && NqLua::FileTime(path.c_str()) != m_FileTime; }
xr_string NqDoc::Text() const			{ return NqLua::Write(quest); }
xr_string NqDoc::Outline() const		{ return NqLua::Outline(quest); }

bool NqDoc::Load(xr_string& err)
{
	xr_string text;
	if (!NqLua::ReadFile(path.c_str(), text, err)) return false;
	SNqQuest q;
	NqLua::SError e;
	xr_vector<xr_string> shape;
	SNqValue v;
	if (!NqLua::ParseValue(text.c_str(), (u32)text.size(), NqUtil::BaseName(path.c_str()).c_str(), v, e))
	{
		err = e.message;
		if (e.line > 0) { char at[32]; sprintf_s(at, " (line %d)", e.line); err += at; }
		return false;
	}
	NqLua::QuestFromValue(v, q, shape);
	quest = q;
	m_Undo.clear(); m_Redo.clear();
	dirty = false;
	m_FileTime = NqLua::FileTime(path.c_str());
	++revision;
	// nodes without coordinates get a place right away (par. 4.5 p.2)
	NqLayout::EnsurePositions(quest);
	Validate();
	PruneSelection();
	// shape problems stay visible until the file is fixed; what they describe is
	// missing from the model, so the document is only part of the file
	m_Partial = !shape.empty();
	xr_vector<SNqProblem> sp;
	NqValidate::ShapeErrors(shape, sp);
	problems.insert(problems.begin(), sp.begin(), sp.end());
	return true;
}

bool NqDoc::SetFromLua(const xr_string& text, xr_string& err)
{
	SNqValue v;
	NqLua::SError e;
	if (!NqLua::ParseValue(text.c_str(), (u32)text.size(), NqUtil::BaseName(path.c_str()).c_str(), v, e))
	{
		err = e.message;
		if (e.line > 0 && e.col > 0)	err += NqUtil::Format(" (line %d:%d)", e.line, e.col);
		else if (e.line > 0)			err += NqUtil::Format(" (line %d)", e.line);
		return false;
	}
	SNqQuest q;
	xr_vector<xr_string> shape;
	NqLua::QuestFromValue(v, q, shape);
	// the reader DROPS what it cannot type (a pin target that is not a string, a
	// node without an id). Taking the text anyway would put a document into the
	// tab that no longer says what the caller wrote, and the next save would make
	// that the file. Refuse instead - the document is left untouched.
	if (!shape.empty())
	{
		err = shape[0];
		for (u32 i = 1; i < shape.size(); ++i) { err += "; "; err += shape[i]; }
		return false;
	}
	Snapshot();
	quest = q;
	m_Partial = false;
	NqLayout::EnsurePositions(quest);
	Changed();
	return true;
}

bool NqDoc::Save(bool force, xr_string& err)
{
	if (!NqDocs::InsideProject(path.c_str())) { err = "the document is outside the project folder - it cannot be saved from here"; return false; }
	if (!force && ModifiedExternally()) { err = "modified externally"; return false; }
	xr_string text = Text();
	if (!NqLua::WriteFile(path.c_str(), text, err)) return false;
	m_FileTime = NqLua::FileTime(path.c_str());
	dirty = false;
	NqDocs::InvalidateProjectIndex();
	return true;
}

bool NqDoc::SaveRescue(xr_string& written, xr_string& err)
{
	written = path;
	if (path.empty()) { err = "the document has no path"; return false; }
	// overwriting a file somebody else changed - or one whose unreadable parts
	// are not in this model - would trade one loss for another
	const bool aside = ModifiedExternally() || m_Partial;
	if (aside) written += ".recovered";
	if (!NqLua::WriteFile(written.c_str(), Text(), err)) return false;
	if (!aside)
	{
		m_FileTime = NqLua::FileTime(path.c_str());
		dirty = false;
	}
	NqDocs::InvalidateProjectIndex();
	return true;
}

bool NqDoc::Reload(xr_string& err)
{
	if (!FileExists()) { err = "file does not exist"; return false; }
	return Load(err);
}

//------------------------------------------------------------------------------
void NqDoc::Snapshot()
{
	m_Undo.push_back(quest);
	if (m_Undo.size() > kUndoRing) m_Undo.erase(m_Undo.begin());
	m_Redo.clear();
}

bool NqDoc::Undo()
{
	if (m_Undo.empty()) return false;
	m_Redo.push_back(quest);
	quest = m_Undo.back();
	m_Undo.pop_back();
	Changed();
	return true;
}

bool NqDoc::Redo()
{
	if (m_Redo.empty()) return false;
	m_Undo.push_back(quest);
	quest = m_Redo.back();
	m_Redo.pop_back();
	Changed();
	return true;
}

void NqDoc::PruneSelection()
{
	// a selection that outlives its nodes is not harmless: RemoveNodes is
	// all-or-nothing, so one dead id kills Delete for everything else, and Copy /
	// Frame / the inspector go quiet the same way
	for (int i = (int)selection.size() - 1; i >= 0; --i)
		if (!quest.FindNode(selection[i].c_str())) selection.erase(selection.begin() + i);
	if (selection.size() != 1) sel_slot.clear();
}

void NqDoc::Changed()
{
	dirty = true;
	++revision;
	quest.InvalidateLookupIndices();
	NqLua::CanonicalizePins(quest);
	// nodes added without coordinates (quest_apply, paste) get a place at once
	NqLayout::EnsurePositions(quest);
	Validate();
	PruneSelection();
}

void NqDoc::Validate()
{
	problems.clear();
	NqDocs::Validate(quest, path.c_str(), problems);
}

int NqDoc::Layout(bool only_missing)
{
	SNqQuest before = quest;
	const int moved = NqLayout::Run(quest, only_missing);
	if (!moved) return 0;
	SNqQuest after = quest;
	quest = before;
	Snapshot();
	quest = after;
	Changed();
	return moved;
}

xr_string NqDoc::FreeNodeId(LPCSTR base) const
{
	xr_string b = (base && base[0]) ? base : "node";
	if (!quest.FindNode(b.c_str())) return b;
	for (int i = 2; i < 10000; ++i)
	{
		// `base` comes from a catalog kind name, which a module can make any
		// length: sprintf_s into a fixed buffer would abort the editor
		const xr_string cand = NqUtil::Format("%s%d", b.c_str(), i);
		if (!quest.FindNode(cand.c_str())) return cand;
	}
	return b;
}

//------------------------------------------------------------------------------
// typed edits
//------------------------------------------------------------------------------
bool NqDoc::AddNode(const SNqNode& n, xr_string& err)
{
	if (n.id.empty())					{ err = "node id is empty"; return false; }
	if (quest.FindNode(n.id.c_str()))	{ err = "node '" + n.id + "' already exists"; return false; }
	Snapshot();
	quest.nodes.push_back(n);
	quest.InvalidateLookupIndices();
	Changed();
	return true;
}

bool NqDoc::RemoveNodes(const xr_vector<xr_string>& ids, xr_string& err)
{
	for (u32 i = 0; i < ids.size(); ++i)
		if (!quest.FindNode(ids[i].c_str())) { err = "node '" + ids[i] + "' does not exist"; return false; }
	if (ids.empty()) return true;
	Snapshot();
	for (u32 i = 0; i < ids.size(); ++i)
	{
		const int idx = quest.NodeIndex(ids[i].c_str());
		if (idx < 0) continue;
		quest.nodes.erase(quest.nodes.begin() + idx);
		quest.InvalidateLookupIndices();
		// edges into the removed node go with it
		for (u32 n = 0; n < quest.nodes.size(); ++n)
			for (int p = (int)quest.nodes[n].out.size() - 1; p >= 0; --p)
			{
				xr_vector<xr_string>& t = quest.nodes[n].out[p].second;
				for (int k = (int)t.size() - 1; k >= 0; --k) if (t[k] == ids[i]) t.erase(t.begin() + k);
				if (t.empty()) quest.nodes[n].out.erase(quest.nodes[n].out.begin() + p);
			}
	}
	Changed();
	return true;
}

// references to a node id inside params: node_done.node / elapsed.node
static void RenameNodeRefs(SNqValue& params, LPCSTR kind, LPCSTR id, LPCSTR new_id)
{
	if (!params.IsTable()) return;
	if ((0 == strcmp(kind, "node_done") || 0 == strcmp(kind, "elapsed")))
		if (SNqValue* n = params.Get("node"))
			if (n->IsString() && n->s == id) n->s = new_id;
	// nested condition lists (any.of, cases)
	for (u32 i = 0; i < params.vals.size(); ++i)
	{
		SNqValue& v = params.vals[i];
		if (!v.IsTable()) continue;
		for (u32 a = 0; a < v.arr.size(); ++a)
		{
			SNqValue& e = v.arr[a];
			if (!e.IsTable()) continue;
			if (SNqValue* p = e.Get("params")) RenameNodeRefs(*p, e.GetString("kind").c_str(), id, new_id);
			if (SNqValue* cl = e.Get("cond"))
				for (u32 k = 0; k < cl->arr.size(); ++k)
					if (SNqValue* p2 = cl->arr[k].Get("params")) RenameNodeRefs(*p2, cl->arr[k].GetString("kind").c_str(), id, new_id);
		}
	}
}

bool NqDoc::RenameNode(LPCSTR id, LPCSTR new_id, xr_string& err)
{
	SNqNode* n = quest.FindNode(id);
	if (!n)								{ err = "node does not exist"; return false; }
	if (!new_id || !new_id[0])			{ err = "new id is empty"; return false; }
	if (0 == strcmp(id, new_id))		return true;
	if (quest.FindNode(new_id))			{ err = xr_string("node '") + new_id + "' already exists"; return false; }
	Snapshot();
	n = quest.FindNode(id);
	n->id = new_id;
	quest.InvalidateLookupIndices();
	for (u32 i = 0; i < quest.nodes.size(); ++i)
	{
		SNqNode& m = quest.nodes[i];
		for (u32 p = 0; p < m.out.size(); ++p)
			for (u32 t = 0; t < m.out[p].second.size(); ++t)
				if (m.out[p].second[t] == id) m.out[p].second[t] = new_id;
		for (u32 k = 0; k < m.cond.size(); ++k)		RenameNodeRefs(m.cond[k].params, m.cond[k].kind.c_str(), id, new_id);
		for (u32 k = 0; k < m.on_enter.size(); ++k)	RenameNodeRefs(m.on_enter[k].params, m.on_enter[k].kind.c_str(), id, new_id);
		for (u32 k = 0; k < m.on_exit.size(); ++k)	RenameNodeRefs(m.on_exit[k].params, m.on_exit[k].kind.c_str(), id, new_id);
		RenameNodeRefs(m.params, m.kind.c_str(), id, new_id);
	}
	for (u32 i = 0; i < selection.size(); ++i) if (selection[i] == id) selection[i] = new_id;
	Changed();
	return true;
}

bool NqDoc::RenameTask(LPCSTR id, LPCSTR new_id, xr_string& err, int& references_updated)
{
	references_updated = 0;
	err.clear();
	if (!id || !id[0]) { err = "task id is empty"; return false; }
	if (!new_id || !new_id[0]) { err = "new task id is empty"; return false; }
	if (!NqText::ValidId(new_id)) { err = "new task id must be [a-z0-9_]+"; return false; }
	int declarations = 0;
	for (u32 i = 0; i < quest.tasks.size(); ++i)
		if (quest.tasks[i].id == id) ++declarations;
	if (!declarations) { err = xr_string("task '") + id + "' does not exist"; return false; }
	if (declarations != 1)
	{
		err = NqUtil::Format("task '%s' is declared %d times; rename requires exactly one source declaration", id, declarations);
		return false;
	}
	if (0 == strcmp(id, new_id)) { err = "new task id is unchanged"; return false; }
	if (quest.FindTask(new_id)) { err = xr_string("task '") + new_id + "' already exists"; return false; }
	if (m_Partial) { err = "task rename refused: the open document is an incomplete model of a malformed source file"; return false; }

	SNqQuest after = quest;
	xr_vector<NqReferences::SDiagnostic> diagnostics;
	if (!NqReferences::RenameTask(after, path.c_str(), id, new_id, references_updated, diagnostics))
	{
		err = "task rename refused because reference coverage is incomplete";
		for (u32 i = 0; i < diagnostics.size(); ++i)
		{
			err += i ? "; " : ": ";
			err += diagnostics[i].Text();
		}
		return false;
	}
	Snapshot();
	quest = std::move(after);
	Changed();
	return true;
}

bool NqDoc::Connect(LPCSTR from, LPCSTR pin, LPCSTR to, xr_string& err)
{
	SNqNode* f = quest.FindNode(from);
	if (!f)						{ err = "source node does not exist"; return false; }
	if (!to || !to[0])			{ err = "target id is empty"; return false; }
	if (!pin || !pin[0])		{ err = "pin is empty"; return false; }
	Snapshot();
	f = quest.FindNode(from);
	f->Connect(pin, to);
	Changed();
	return true;
}

bool NqDoc::Disconnect(LPCSTR from, LPCSTR pin, LPCSTR to, xr_string& err)
{
	SNqNode* f = quest.FindNode(from);
	if (!f)						{ err = "source node does not exist"; return false; }
	Snapshot();
	f = quest.FindNode(from);
	if (to && to[0]) f->Disconnect(pin, to);
	else
		for (int p = (int)f->out.size() - 1; p >= 0; --p)
			if (!pin || !pin[0] || f->out[p].first == pin) f->out.erase(f->out.begin() + p);
	Changed();
	return true;
}

bool NqDoc::SetPos(LPCSTR id, float x, float y, bool snapshot)
{
	SNqNode* n = quest.FindNode(id);
	if (!n) return false;
	if (snapshot) Snapshot();
	n = quest.FindNode(id);
	// a drag never goes through Changed(), so the bound is applied here too
	n->pos.set(NqLayout::Sane(x), NqLayout::Sane(y));
	n->has_pos = true;
	dirty = true;
	++revision;
	return true;
}

//------------------------------------------------------------------------------
// operations (quest_apply)
//------------------------------------------------------------------------------
namespace
{
	int SlotIndex(const SNqValue& op, LPCSTR key, int size, bool for_insert)
	{
		// 1-based in the protocol; absent = append (insert) / invalid (others)
		const SNqValue* v = op.Get(key);
		if (!v || v->IsNil()) return for_insert ? size : -1;
		const int i = (int)v->AsNumber(0) - 1;
		if (i < 0) return -1;
		if (for_insert) return (i > size) ? size : i;
		return (i >= size) ? -1 : i;
	}

	bool SlotOf(const SNqValue& op, LPCSTR key, SNqNode& n, xr_vector<SNqAction>*& lst, xr_string& err)
	{
		xr_string slot = op.GetString(key, "enter");
		if (slot == "enter" || slot == "on_enter")		lst = &n.on_enter;
		else if (slot == "exit" || slot == "on_exit")	lst = &n.on_exit;
		else { err = "slot must be \"enter\" or \"exit\""; return false; }
		return true;
	}
}

bool NqDoc::ApplyOne(const SNqValue& op, xr_string& err)
{
	if (!op.IsTable()) { err = "operation must be a table { op = \"...\", ... }"; return false; }
	const xr_string what = op.GetString("op");
	if (what.empty()) { err = "operation without op"; return false; }

	if (what == "add_node")
	{
		const SNqValue* nv = op.Get("node");
		if (!nv) { err = "add_node needs node = { id = ..., kind = ..., ... }"; return false; }
		SNqNode n;
		xr_vector<xr_string> shape;
		if (!NqLua::NodeFromValue(*nv, n, shape) || !shape.empty()) { err = shape.empty() ? "bad node" : shape[0]; return false; }
		if (quest.FindNode(n.id.c_str())) { err = "node '" + n.id + "' already exists"; return false; }
		quest.nodes.push_back(n);
		quest.InvalidateLookupIndices();
		return true;
	}
	if (what == "remove_node")
	{
		const xr_string id = op.GetString("id");
		if (!quest.FindNode(id.c_str())) { err = "node '" + id + "' does not exist"; return false; }
		xr_vector<xr_string> ids; ids.push_back(id);
		// inline (no snapshot: the caller owns it)
		const int idx = quest.NodeIndex(id.c_str());
		quest.nodes.erase(quest.nodes.begin() + idx);
		quest.InvalidateLookupIndices();
		for (u32 n = 0; n < quest.nodes.size(); ++n)
			for (int p = (int)quest.nodes[n].out.size() - 1; p >= 0; --p)
			{
				xr_vector<xr_string>& t = quest.nodes[n].out[p].second;
				for (int k = (int)t.size() - 1; k >= 0; --k) if (t[k] == id) t.erase(t.begin() + k);
				if (t.empty()) quest.nodes[n].out.erase(quest.nodes[n].out.begin() + p);
			}
		return true;
	}
	if (what == "rename_node")
	{
		const xr_string id = op.GetString("id"), new_id = op.GetString("new_id");
		SNqNode* n = quest.FindNode(id.c_str());
		if (!n) { err = "node '" + id + "' does not exist"; return false; }
		if (new_id.empty()) { err = "rename_node needs new_id"; return false; }
		if (id == new_id) return true;
		if (quest.FindNode(new_id.c_str())) { err = "node '" + new_id + "' already exists"; return false; }
		n->id = new_id;
		quest.InvalidateLookupIndices();
		for (u32 i = 0; i < quest.nodes.size(); ++i)
		{
			SNqNode& m = quest.nodes[i];
			for (u32 p = 0; p < m.out.size(); ++p)
				for (u32 t = 0; t < m.out[p].second.size(); ++t)
					if (m.out[p].second[t] == id) m.out[p].second[t] = new_id;
			for (u32 k = 0; k < m.cond.size(); ++k)		RenameNodeRefs(m.cond[k].params, m.cond[k].kind.c_str(), id.c_str(), new_id.c_str());
			for (u32 k = 0; k < m.on_enter.size(); ++k)	RenameNodeRefs(m.on_enter[k].params, m.on_enter[k].kind.c_str(), id.c_str(), new_id.c_str());
			for (u32 k = 0; k < m.on_exit.size(); ++k)	RenameNodeRefs(m.on_exit[k].params, m.on_exit[k].kind.c_str(), id.c_str(), new_id.c_str());
			RenameNodeRefs(m.params, m.kind.c_str(), id.c_str(), new_id.c_str());
		}
		return true;
	}
	if (what == "set_node")
	{
		const xr_string id = op.GetString("id");
		SNqNode* n = quest.FindNode(id.c_str());
		if (!n) { err = "node '" + id + "' does not exist"; return false; }
		const SNqValue* patch = op.Get("patch");
		if (!patch || !patch->IsTable()) { err = "set_node needs patch = { ... }"; return false; }
		// each given field replaces the node's field: kind, once ("default" =
		// unset), params, cond, on_enter, on_exit, out, pos ({} = clear), comment
		xr_vector<xr_string> shape;
		for (u32 i = 0; i < patch->keys.size(); ++i)
		{
			const xr_string& k = patch->keys[i];
			const SNqValue& v = patch->vals[i];
			if (k == "id")	{ err = "set_node cannot change id - use rename_node"; return false; }
			else if (k == "kind")	{ if (!v.IsString()) { err = "kind must be a string"; return false; } n->kind = v.s; }
			else if (k == "once")
			{
				if (v.IsString() && v.s == "default") n->once_set = false;
				else { n->once = v.AsBool(false); n->once_set = true; }
			}
			else if (k == "params")	{ if (!v.IsTable() && !v.IsNil()) { err = "params must be a table"; return false; } n->params = v; }
			else if (k == "comment"){ n->comment = v.AsString(); }
			else if (k == "pos")
			{
				if (v.IsTable() && v.arr.size() >= 2) { n->pos.set((float)v.arr[0].AsNumber(), (float)v.arr[1].AsNumber()); n->has_pos = true; }
				else n->has_pos = false;
			}
			else if (k == "cond" || k == "on_enter" || k == "on_exit" || k == "out")
			{
				// reuse the node reader on a minimal table
				SNqValue tmp = SNqValue::Table();
				tmp.Set("id", SNqValue::String(n->id));
				tmp.Set("kind", SNqValue::String(n->kind));
				tmp.Set(k.c_str(), v);
				SNqNode parsed;
				if (!NqLua::NodeFromValue(tmp, parsed, shape) || !shape.empty()) { err = shape.empty() ? "bad " + k : shape[0]; return false; }
				if (k == "cond")			n->cond = parsed.cond;
				else if (k == "on_enter")	n->on_enter = parsed.on_enter;
				else if (k == "on_exit")	n->on_exit = parsed.on_exit;
				else						n->out = parsed.out;
			}
			else { err = "set_node: unknown field '" + k + "'"; return false; }
		}
		return true;
	}
	if (what == "connect" || what == "disconnect")
	{
		const xr_string from = op.GetString("from"), pin = op.GetString("pin", "next"), to = op.GetString("to");
		SNqNode* f = quest.FindNode(from.c_str());
		if (!f) { err = "node '" + from + "' does not exist"; return false; }
		if (what == "connect")
		{
			// a missing target or an undeclared pin is a validation problem
			// (E007), not a refused edit: the author sees it in the problems list
			if (to.empty()) { err = "connect needs to = <node id>"; return false; }
			f->Connect(pin.c_str(), to.c_str());
		}
		else
		{
			if (!to.empty()) f->Disconnect(pin.c_str(), to.c_str());
			else
				for (int p = (int)f->out.size() - 1; p >= 0; --p)
					if (f->out[p].first == pin) f->out.erase(f->out.begin() + p);
		}
		return true;
	}
	if (what == "add_action" || what == "set_action" || what == "remove_action" || what == "move_action")
	{
		const xr_string id = op.GetString("id");
		SNqNode* n = quest.FindNode(id.c_str());
		if (!n) { err = "node '" + id + "' does not exist"; return false; }
		xr_vector<SNqAction>* lst = 0;
		if (!SlotOf(op, "slot", *n, lst, err)) return false;
		if (what == "add_action")
		{
			const SNqValue* av = op.Get("action");
			SNqAction a; xr_string e;
			if (!av || !NqLua::ActionFromValue(*av, a, e)) { err = e.empty() ? "add_action needs action = { kind = ..., params = { ... } }" : e; return false; }
			const int at = SlotIndex(op, "index", (int)lst->size(), true);
			if (at < 0) { err = "index out of range (1-based)"; return false; }
			lst->insert(lst->begin() + at, a);
			return true;
		}
		const int at = SlotIndex(op, "index", (int)lst->size(), false);
		if (at < 0) { err = "index out of range (1-based)"; return false; }
		if (what == "remove_action") { lst->erase(lst->begin() + at); return true; }
		if (what == "set_action")
		{
			const SNqValue* patch = op.Get("patch");
			if (!patch || !patch->IsTable()) { err = "set_action needs patch = { kind = ..., params = { ... } }"; return false; }
			SNqAction& a = (*lst)[at];
			if (const SNqValue* k = patch->Get("kind"))		{ if (!k->IsString()) { err = "kind must be a string"; return false; } a.kind = k->s; }
			if (const SNqValue* p = patch->Get("params"))	{ if (!p->IsTable() && !p->IsNil()) { err = "params must be a table"; return false; } a.params = *p; }
			return true;
		}
		// move_action: to_id / to_slot / to_index (all optional)
		SNqAction moving = (*lst)[at];
		lst->erase(lst->begin() + at);
		SNqNode* target = n;
		if (op.Has("to_id"))
		{
			target = quest.FindNode(op.GetString("to_id").c_str());
			if (!target) { err = "target node does not exist"; return false; }
		}
		xr_vector<SNqAction>* dst = lst;
		if (op.Has("to_slot") || target != n)
		{
			SNqValue tmp = SNqValue::Table();
			tmp.Set("slot", SNqValue::String(op.GetString("to_slot", op.GetString("slot", "enter").c_str())));
			if (!SlotOf(tmp, "slot", *target, dst, err)) return false;
		}
		int to = SlotIndex(op, "to_index", (int)dst->size(), true);
		if (to < 0) { err = "to_index out of range (1-based)"; return false; }
		dst->insert(dst->begin() + to, moving);
		return true;
	}
	if (what == "set_quest")
	{
		const SNqValue* patch = op.Get("patch");
		if (!patch || !patch->IsTable()) { err = "set_quest needs patch = { id/title/activation/vars/tasks }"; return false; }
		for (u32 i = 0; i < patch->keys.size(); ++i)
		{
			const xr_string& k = patch->keys[i];
			const SNqValue& v = patch->vals[i];
			if (k == "id")				quest.id = v.AsString();
			else if (k == "title")		quest.title = v;
			else if (k == "activation")	quest.activation = v.AsString();
			else if (k == "nq")			quest.nq = (int)v.AsNumber(1);
			else if (k == "vars")
			{
				if (!v.IsTable()) { err = "vars must be a table"; return false; }
				quest.vars.clear();
				for (u32 j = 0; j < v.keys.size(); ++j) { SNqVar var; var.name = v.keys[j]; var.value = v.vals[j]; quest.vars.push_back(var); }
			}
			else if (k == "tasks")
			{
				if (!v.IsTable()) { err = "tasks must be a table"; return false; }
				quest.tasks.clear();
				for (u32 j = 0; j < v.keys.size(); ++j) { SNqTask t; NqLua::TaskFromValue(v.keys[j].c_str(), v.vals[j], t); quest.tasks.push_back(t); }
			}
			else { err = "set_quest: unknown field '" + k + "'"; return false; }
		}
		quest.InvalidateLookupIndices();
		return true;
	}
	if (what == "set_pos")
	{
		const xr_string id = op.GetString("id");
		SNqNode* n = quest.FindNode(id.c_str());
		if (!n) { err = "node '" + id + "' does not exist"; return false; }
		n->pos.set((float)op.GetNumber("x"), (float)op.GetNumber("y"));
		n->has_pos = true;
		return true;
	}
	err = "unknown op '" + what + "'";
	return false;
}

bool NqDoc::Apply(const SNqValue& op, xr_string& err)
{
	SNqQuest backup = quest;
	if (!ApplyOne(op, err)) { quest = backup; return false; }
	SNqQuest after = quest;
	quest = backup;
	Snapshot();
	quest = after;
	Changed();
	return true;
}

bool NqDoc::ApplyOps(const SNqValue& ops, xr_string& err, int& applied)
{
	applied = 0;
	if (!ops.IsTable()) { err = "ops must be a list of operations"; return false; }
	SNqQuest backup = quest;
	// a single op passed without the outer list is accepted
	if (ops.arr.empty() && ops.Has("op"))
	{
		if (!ApplyOne(ops, err)) { quest = backup; return false; }
		applied = 1;
	}
	else
		for (u32 i = 0; i < ops.arr.size(); ++i)
		{
			xr_string e;
			if (!ApplyOne(ops.arr[i], e))
			{
				quest = backup;
				char at[48]; sprintf_s(at, "op #%u: ", i + 1);
				err = at; err += e;
				return false;
			}
			++applied;
		}
	if (!applied) return true;
	SNqQuest after = quest;
	quest = backup;
	Snapshot();
	quest = after;
	Changed();
	return true;
}

//==============================================================================
// NqDocs registry
//==============================================================================
namespace
{
	xr_vector<NqDoc*>	s_Docs;
}

bool NqDocs::InsideProject(LPCSTR abs)
{
	if (!EditorProject::Active() || !abs) return false;
	xr_string root = NqUtil::PathKey(EditorProject::Root());
	xr_string p = NqUtil::PathKey(abs);
	return p.size() > root.size() + 1 && 0 == strncmp(p.c_str(), root.c_str(), root.size()) && p[root.size()] == '\\';
}

bool NqDocs::Resolve(LPCSTR path, xr_string& abs, xr_string& err, bool for_write)
{
	if (!NqUtil::ProjectPath(path, abs, err)) return false;
	if (NqUtil::Extension(abs.c_str()).empty()) abs += ".nqasset";
	if (for_write && !InsideProject(abs.c_str())) { err = "path is outside the project folder - quests are written into the project only"; return false; }
	return true;
}

NqDoc* NqDocs::Get(LPCSTR path)
{
	xr_string abs, err;
	if (!Resolve(path, abs, err)) return 0;
	const xr_string key = NqUtil::PathKey(abs.c_str());
	for (u32 i = 0; i < s_Docs.size(); ++i)
		if (NqUtil::PathKey(s_Docs[i]->path.c_str()) == key) return s_Docs[i];
	return 0;
}

NqDoc* NqDocs::Open(LPCSTR path, xr_string& err)
{
	xr_string abs;
	if (!Resolve(path, abs, err)) return 0;
	if (NqDoc* d = Get(abs.c_str())) return d;
	if (!NqUtil::FileExists(abs.c_str())) { err = "file does not exist: " + NqUtil::ProjectRelative(abs.c_str()); return 0; }
	NqDoc* d = xr_new<NqDoc>();
	d->path = abs;
	if (!d->Load(err)) { xr_delete(d); return 0; }
	s_Docs.push_back(d);
	return d;
}

NqDoc* NqDocs::Create(LPCSTR path, xr_string& err)
{
	xr_string abs;
	if (!Resolve(path, abs, err)) return 0;
	if (NqDoc* d = Get(abs.c_str())) return d;
	NqDoc* d = xr_new<NqDoc>();
	d->path = abs;
	d->quest.id = NqUtil::BaseName(abs.c_str());
	d->dirty = true;
	s_Docs.push_back(d);
	return d;
}

bool NqDocs::IsOpen(const NqDoc* d)
{
	for (u32 i = 0; i < s_Docs.size(); ++i)
		if (s_Docs[i] == d) return true;
	return false;
}

bool NqDocs::Close(LPCSTR path, bool discard, xr_string& err)
{
	NqDoc* d = Get(path);
	if (!d) { err = "not open"; return false; }
	if (d->dirty && !discard) { err = "unsaved"; return false; }
	for (u32 i = 0; i < s_Docs.size(); ++i)
		if (s_Docs[i] == d) { s_Docs.erase(s_Docs.begin() + i); break; }
	xr_delete(d);
	return true;
}

void NqDocs::All(xr_vector<NqDoc*>& out)	{ out = s_Docs; }

int NqDocs::SaveDirty()
{
	int saved = 0;
	for (u32 i = 0; i < s_Docs.size(); ++i)
	{
		NqDoc* d = s_Docs[i];
		if (!d->dirty) continue;
		xr_string written, err;
		if (d->SaveRescue(written, err))
		{
			++saved;
			if (written == d->path)	Msg("* [nq] unsaved quest written before it was closed: %s", d->path.c_str());
			else					Msg("! [nq] %s changed on disk - the unsaved changes went to %s", d->path.c_str(), written.c_str());
		}
		else
			Msg("! [nq] %s could NOT be saved (%s) - the unsaved changes are lost", d->path.c_str(), err.c_str());
	}
	return saved;
}

void NqDocs::CloseAll()
{
	// nothing here asks: this runs on the project switch and on shutdown, where a
	// modal would stall MCP and -nodlg. Losing the edits is not an option either,
	// so they go to disk first.
	SaveDirty();
	for (u32 i = 0; i < s_Docs.size(); ++i) xr_delete(s_Docs[i]);
	s_Docs.clear();
}

bool NqDocs::ReadQuest(LPCSTR path, SNqQuest& q, xr_string& err, bool disk_only)
{
	xr_string abs;
	if (!Resolve(path, abs, err)) return false;
	if (!disk_only)
		if (NqDoc* d = Get(abs.c_str())) { q = d->quest; return true; }
	xr_string text;
	if (!NqLua::ReadFile(abs.c_str(), text, err)) { err += ": " + NqUtil::ProjectRelative(abs.c_str()); return false; }
	NqLua::SError e;
	if (!NqLua::ParseQuest(text.c_str(), (u32)text.size(), NqUtil::BaseName(abs.c_str()).c_str(), q, e))
	{
		err = e.message;
		if (e.line > 0) { char at[32]; sprintf_s(at, " (line %d)", e.line); err += at; }
		return false;
	}
	return true;
}

xr_string NqDocs::CurrentLevel()
{
	if (!Tools || Tools->m_LastFileName.empty()) return xr_string();
	xr_string name = NqUtil::BaseName(Tools->m_LastFileName.c_str());
	// only a level the linked game knows can pin the actor to a level
	if (!NqPickers::Available() || !NqPickers::Known(NqPickers::tLevel, name.c_str())) return xr_string();
	return name;
}

void NqDocs::Validate(const SNqQuest& q, LPCSTR path, xr_vector<SNqProblem>& out, bool disk_only)
{
	xr_vector<xr_string> others;
	OtherQuestIds(path, others, disk_only);
	xr_string level = CurrentLevel();
	NqValidate::SContext ctx;
	ctx.file_path = path;
	ctx.project_quest_ids = &others;
	ctx.level = level.empty() ? 0 : level.c_str();
	ctx.use_pickers = true;
	NqValidate::Run(q, ctx, out);
}

void NqDocs::InvalidateProjectIndex()	{ NqProjectIndex::Invalidate(); }

void NqDocs::ProjectQuests(xr_vector<SProjectQuest>& out, bool disk_only)
{
	out.clear();
	NqProjectIndex::SSnapshot snapshot;
	xr_string index_error;
	if (!NqProjectIndex::Snapshot(snapshot, index_error, !disk_only)) return;
	for (u32 i = 0; i < snapshot.entries.size(); ++i)
	{
		const NqProjectIndex::SEntry& entry = snapshot.entries[i];
		SProjectQuest pq;
		pq.path = entry.path;
		pq.id = entry.readable ? entry.quest.id : "";
		pq.title = entry.readable ? NqText::Preview(entry.quest.title) : "";
		pq.readable = entry.readable && entry.complete;
		pq.error = entry.error;
		out.push_back(pq);
	}
}

void NqDocs::OtherQuestIds(LPCSTR except_path, xr_vector<xr_string>& out, bool disk_only)
{
	out.clear();
	xr_vector<SProjectQuest> all;
	ProjectQuests(all, disk_only);
	const xr_string me = except_path ? NqUtil::PathKey(except_path) : xr_string();
	for (u32 i = 0; i < all.size(); ++i)
	{
		if (!all[i].readable || all[i].id.empty()) continue;
		if (!me.empty() && NqUtil::PathKey(all[i].path.c_str()) == me) continue;
		out.push_back(all[i].id);
	}
}
