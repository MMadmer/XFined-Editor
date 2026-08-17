#include "stdafx.h"
#include "NqAsset.h"
#include "NqUtil.h"
#include "NqCatalog.h"

//------------------------------------------------------------------------------
// canonical key order. One fixed list for every generic table so that the
// writer never depends on the catalog being present: well-known keys in the
// order the catalog declares its parameters (and nested refs read naturally),
// unknown keys alphabetically after them.
//------------------------------------------------------------------------------
static const char* kKeyOrder[] =
{
	"kind", "name", "npc", "who", "task", "quest", "target", "section",
	"story", "profile", "community", "level", "place", "into", "smart", "spawn",
	"restrictor", "ref", "hold", "pos", "radius", "x", "y", "z",
	"text", "title", "descr", "initiator", "sender", "icon", "spot",
	"op", "value", "delta", "is", "by_actor", "count", "amount", "info",
	"map_spot", "spot_text", "follow_actor", "allow_break",
	"status", "restart", "repeat", "cooldown", "timeout",
	"since", "node", "duration", "seconds", "game_hours", "game_minutes",
	"from", "to", "percent", "theme", "code", "of", "cond", "weight",
	"params", "not", "rus", "eng", "ukr", "pol", "fra", "ger", "ita", "spa",
};

int NqKeyRank(LPCSTR key)
{
	if (!key) return 100000;
	for (int i = 0; i < (int)(sizeof(kKeyOrder) / sizeof(kKeyOrder[0])); ++i)
		if (0 == strcmp(key, kKeyOrder[i])) return i;
	return 100000;
}

bool NqKeyLess(const xr_string& a, const xr_string& b)
{
	const int ra = NqKeyRank(a.c_str()), rb = NqKeyRank(b.c_str());
	if (ra != rb) return ra < rb;
	return a < b;
}

//------------------------------------------------------------------------------
// SNqValue
//------------------------------------------------------------------------------
const SNqValue* SNqValue::Get(LPCSTR key) const
{
	if (type != tTable || !key) return 0;
	for (u32 i = 0; i < keys.size(); ++i)
		if (keys[i] == key) return &vals[i];
	return 0;
}

SNqValue* SNqValue::Get(LPCSTR key)
{
	if (type != tTable || !key) return 0;
	for (u32 i = 0; i < keys.size(); ++i)
		if (keys[i] == key) return &vals[i];
	return 0;
}

SNqValue& SNqValue::Set(LPCSTR key)
{
	if (type == tNil) type = tTable;
	R_ASSERT(type == tTable);
	for (u32 i = 0; i < keys.size(); ++i)
		if (keys[i] == key) return vals[i];
	// keep the canonical order on insert so equality and the writer agree
	xr_string k = key;
	u32 at = 0;
	while (at < keys.size() && NqKeyLess(keys[at], k)) ++at;
	keys.insert(keys.begin() + at, k);
	vals.insert(vals.begin() + at, SNqValue());
	return vals[at];
}

SNqValue& SNqValue::Set(LPCSTR key, const SNqValue& v)
{
	SNqValue& slot = Set(key);
	slot = v;
	return slot;
}

void SNqValue::Erase(LPCSTR key)
{
	if (type != tTable) return;
	for (u32 i = 0; i < keys.size(); ++i)
		if (keys[i] == key)
		{
			keys.erase(keys.begin() + i);
			vals.erase(vals.begin() + i);
			return;
		}
}

SNqValue& SNqValue::Push(const SNqValue& v)
{
	if (type == tNil) type = tTable;
	arr.push_back(v);
	return arr.back();
}

xr_string SNqValue::AsString(LPCSTR def) const
{
	if (type == tString) return s;
	if (type == tNumber)
	{
		char buf[64];
		if (n == (double)(long long)n && fabs(n) < 9.0e15) sprintf_s(buf, "%lld", (long long)n);
		else sprintf_s(buf, "%.15g", n);
		return xr_string(buf);
	}
	if (type == tBool) return xr_string(b ? "true" : "false");
	return xr_string(def ? def : "");
}

double SNqValue::AsNumber(double def) const
{
	if (type == tNumber) return n;
	if (type == tString && !s.empty())
	{
		char* end = 0;
		const double v = strtod(s.c_str(), &end);
		if (end && *end == 0) return v;
	}
	if (type == tBool) return b ? 1.0 : 0.0;
	return def;
}

bool SNqValue::AsBool(bool def) const
{
	if (type == tBool) return b;
	if (type == tNumber) return n != 0.0;
	if (type == tString) return s == "true" || s == "1";
	return def;
}

xr_string SNqValue::GetString(LPCSTR key, LPCSTR def) const
{
	const SNqValue* v = Get(key);
	return v ? v->AsString(def) : xr_string(def ? def : "");
}

double SNqValue::GetNumber(LPCSTR key, double def) const
{
	const SNqValue* v = Get(key);
	return v ? v->AsNumber(def) : def;
}

bool SNqValue::GetBool(LPCSTR key, bool def) const
{
	const SNqValue* v = Get(key);
	return v ? v->AsBool(def) : def;
}

bool SNqValue::Equals(const SNqValue& o) const
{
	if (type != o.type) return false;
	switch (type)
	{
	case tNil:		return true;
	case tBool:		return b == o.b;
	case tNumber:	return n == o.n;
	case tString:	return s == o.s;
	case tTable:
		if (arr.size() != o.arr.size() || keys.size() != o.keys.size()) return false;
		for (u32 i = 0; i < arr.size(); ++i)	if (!arr[i].Equals(o.arr[i])) return false;
		for (u32 i = 0; i < keys.size(); ++i)
		{
			if (keys[i] != o.keys[i]) return false;
			if (!vals[i].Equals(o.vals[i])) return false;
		}
		return true;
	}
	return false;
}

void SNqValue::Canonicalize()
{
	if (type != tTable) return;
	// insertion sort keeps keys/vals in step (tables are tiny)
	for (u32 i = 1; i < keys.size(); ++i)
	{
		u32 j = i;
		while (j > 0 && NqKeyLess(keys[j], keys[j - 1]))
		{
			std::swap(keys[j], keys[j - 1]);
			std::swap(vals[j], vals[j - 1]);
			--j;
		}
	}
	for (u32 i = 0; i < arr.size(); ++i)	arr[i].Canonicalize();
	for (u32 i = 0; i < vals.size(); ++i)	vals[i].Canonicalize();
}

void SNqValue::Patch(const SNqValue& patch)
{
	if (patch.type != tTable) return;
	if (type == tNil) type = tTable;
	if (type != tTable) return;
	for (u32 i = 0; i < patch.keys.size(); ++i)
	{
		if (patch.vals[i].IsNil())	Erase(patch.keys[i].c_str());
		else						Set(patch.keys[i].c_str(), patch.vals[i]);
	}
	// an array part in the patch replaces ours wholesale (lists are values)
	if (!patch.arr.empty()) arr = patch.arr;
}

//------------------------------------------------------------------------------
// SNqNode
//------------------------------------------------------------------------------
const xr_vector<xr_string>* SNqNode::Targets(LPCSTR pin) const
{
	for (u32 i = 0; i < out.size(); ++i)
		if (out[i].first == pin) return &out[i].second;
	return 0;
}

xr_vector<xr_string>& SNqNode::TargetsOf(LPCSTR pin)
{
	for (u32 i = 0; i < out.size(); ++i)
		if (out[i].first == pin) return out[i].second;
	out.push_back(SNqPin(xr_string(pin), xr_vector<xr_string>()));
	return out.back().second;
}

bool SNqNode::Connect(LPCSTR pin, LPCSTR to)
{
	xr_vector<xr_string>& t = TargetsOf(pin);
	for (u32 i = 0; i < t.size(); ++i) if (t[i] == to) return false;
	t.push_back(xr_string(to));
	return true;
}

bool SNqNode::Disconnect(LPCSTR pin, LPCSTR to)
{
	for (u32 i = 0; i < out.size(); ++i)
	{
		if (out[i].first != pin) continue;
		xr_vector<xr_string>& t = out[i].second;
		for (u32 j = 0; j < t.size(); ++j)
			if (t[j] == to)
			{
				t.erase(t.begin() + j);
				// a pin without targets is not written, so it is not kept either
				if (t.empty()) out.erase(out.begin() + i);
				return true;
			}
		return false;
	}
	return false;
}

bool SNqNode::HasTarget(LPCSTR to) const
{
	for (u32 i = 0; i < out.size(); ++i)
		for (u32 j = 0; j < out[i].second.size(); ++j)
			if (out[i].second[j] == to) return true;
	return false;
}

u32 SNqNode::OutDegree() const
{
	u32 n = 0;
	for (u32 i = 0; i < out.size(); ++i) n += (u32)out[i].second.size();
	return n;
}

xr_vector<SNqAction>& SNqNode::Slot(LPCSTR slot)
{
	return (slot && (0 == _stricmp(slot, "exit") || 0 == _stricmp(slot, "on_exit"))) ? on_exit : on_enter;
}

const xr_vector<SNqAction>* SNqNode::SlotC(LPCSTR slot) const
{
	if (!slot) return 0;
	if (0 == _stricmp(slot, "enter") || 0 == _stricmp(slot, "on_enter")) return &on_enter;
	if (0 == _stricmp(slot, "exit")  || 0 == _stricmp(slot, "on_exit"))  return &on_exit;
	return 0;
}

//------------------------------------------------------------------------------
// SNqQuest
//------------------------------------------------------------------------------
namespace
{
	void SwapValue(SNqValue& left, SNqValue& right) noexcept
	{
		std::swap(left.type, right.type);
		std::swap(left.b, right.b);
		std::swap(left.n, right.n);
		left.s.swap(right.s);
		left.arr.swap(right.arr);
		left.keys.swap(right.keys);
		left.vals.swap(right.vals);
	}

	void SwapQuestPayload(SNqQuest& left, SNqQuest& right) noexcept
	{
		std::swap(left.nq, right.nq);
		left.id.swap(right.id);
		SwapValue(left.title, right.title);
		left.activation.swap(right.activation);
		left.vars.swap(right.vars);
		left.tasks.swap(right.tasks);
		left.nodes.swap(right.nodes);
	}
}

SNqQuest::SNqQuest() : nq(1), activation("auto"), m_LookupRevision(1)
{
}

SNqQuest::SNqQuest(const SNqQuest& other)
	: nq(other.nq), id(other.id), title(other.title), activation(other.activation), vars(other.vars),
	  tasks(other.tasks), nodes(other.nodes), m_LookupRevision(1)
{
}

SNqQuest::SNqQuest(SNqQuest&& other) noexcept : SNqQuest()
{
	SwapQuestPayload(*this, other);
	other.ResetLookupIndices();
}

SNqQuest& SNqQuest::operator=(const SNqQuest& other)
{
	if (this == &other) return *this;
	nq = other.nq;
	id = other.id;
	title = other.title;
	activation = other.activation;
	vars = other.vars;
	tasks = other.tasks;
	nodes = other.nodes;
	ResetLookupIndices();
	return *this;
}

SNqQuest& SNqQuest::operator=(SNqQuest&& other) noexcept
{
	if (this == &other) return *this;
	SNqQuest moved(std::move(other));
	SwapQuestPayload(*this, moved);
	ResetLookupIndices();
	return *this;
}

void SNqQuest::ResetLookupIndices()
{
	m_LookupRevision = 1;
	m_NodeIndex.entries.clear();
	m_NodeIndex.storage = nullptr;
	m_NodeIndex.count = m_NodeIndex.revision = 0;
	m_TaskIndex.entries.clear();
	m_TaskIndex.storage = nullptr;
	m_TaskIndex.count = m_TaskIndex.revision = 0;
	m_VarIndex.entries.clear();
	m_VarIndex.storage = nullptr;
	m_VarIndex.count = m_VarIndex.revision = 0;
}

void SNqQuest::InvalidateLookupIndices()
{
	++m_LookupRevision;
	if (!m_LookupRevision) ResetLookupIndices();
}

void SNqQuest::EnsureNodeIndex() const
{
	const void* storage = nodes.data();
	if (m_NodeIndex.revision == m_LookupRevision && m_NodeIndex.storage == storage && m_NodeIndex.count == nodes.size()) return;
	m_NodeIndex.entries.clear();
	m_NodeIndex.entries.reserve(nodes.size());
	for (u32 i = 0; i < nodes.size(); ++i) m_NodeIndex.entries.emplace(nodes[i].id, int(i));
	m_NodeIndex.storage = storage;
	m_NodeIndex.count = nodes.size();
	m_NodeIndex.revision = m_LookupRevision;
}

void SNqQuest::EnsureTaskIndex() const
{
	const void* storage = tasks.data();
	if (m_TaskIndex.revision == m_LookupRevision && m_TaskIndex.storage == storage && m_TaskIndex.count == tasks.size()) return;
	m_TaskIndex.entries.clear();
	m_TaskIndex.entries.reserve(tasks.size());
	for (u32 i = 0; i < tasks.size(); ++i) m_TaskIndex.entries.emplace(tasks[i].id, int(i));
	m_TaskIndex.storage = storage;
	m_TaskIndex.count = tasks.size();
	m_TaskIndex.revision = m_LookupRevision;
}

void SNqQuest::EnsureVarIndex() const
{
	const void* storage = vars.data();
	if (m_VarIndex.revision == m_LookupRevision && m_VarIndex.storage == storage && m_VarIndex.count == vars.size()) return;
	m_VarIndex.entries.clear();
	m_VarIndex.entries.reserve(vars.size());
	for (u32 i = 0; i < vars.size(); ++i) m_VarIndex.entries.emplace(vars[i].name, int(i));
	m_VarIndex.storage = storage;
	m_VarIndex.count = vars.size();
	m_VarIndex.revision = m_LookupRevision;
}

void SNqQuest::Clear()
{
	nq = 1; id.clear(); title = SNqValue(); activation = "auto";
	vars.clear(); tasks.clear(); nodes.clear();
	InvalidateLookupIndices();
}

SNqNode* SNqQuest::FindNode(LPCSTR id_)
{
	if (!id_) return 0;
	EnsureNodeIndex();
	const auto found = m_NodeIndex.entries.find(id_);
	return found != m_NodeIndex.entries.end() ? &nodes[found->second] : nullptr;
}

const SNqNode* SNqQuest::FindNode(LPCSTR id_) const
{
	if (!id_) return 0;
	EnsureNodeIndex();
	const auto found = m_NodeIndex.entries.find(id_);
	return found != m_NodeIndex.entries.end() ? &nodes[found->second] : nullptr;
}

int SNqQuest::NodeIndex(LPCSTR id_) const
{
	if (!id_) return -1;
	EnsureNodeIndex();
	const auto found = m_NodeIndex.entries.find(id_);
	return found != m_NodeIndex.entries.end() ? found->second : -1;
}

SNqTask* SNqQuest::FindTask(LPCSTR id_)
{
	if (!id_) return 0;
	EnsureTaskIndex();
	const auto found = m_TaskIndex.entries.find(id_);
	return found != m_TaskIndex.entries.end() ? &tasks[found->second] : nullptr;
}

const SNqTask* SNqQuest::FindTask(LPCSTR id_) const
{
	if (!id_) return 0;
	EnsureTaskIndex();
	const auto found = m_TaskIndex.entries.find(id_);
	return found != m_TaskIndex.entries.end() ? &tasks[found->second] : nullptr;
}

SNqVar* SNqQuest::FindVar(LPCSTR name)
{
	if (!name) return 0;
	EnsureVarIndex();
	const auto found = m_VarIndex.entries.find(name);
	return found != m_VarIndex.entries.end() ? &vars[found->second] : nullptr;
}

const SNqVar* SNqQuest::FindVar(LPCSTR name) const
{
	if (!name) return 0;
	EnsureVarIndex();
	const auto found = m_VarIndex.entries.find(name);
	return found != m_VarIndex.entries.end() ? &vars[found->second] : nullptr;
}

int SNqQuest::InDegree(LPCSTR id_) const
{
	int n = 0;
	for (u32 i = 0; i < nodes.size(); ++i)
		for (u32 p = 0; p < nodes[i].out.size(); ++p)
			for (u32 t = 0; t < nodes[i].out[p].second.size(); ++t)
				if (nodes[i].out[p].second[t] == id_) ++n;
	return n;
}

void SNqQuest::Reachable(xr_vector<bool>& out, LPCSTR from) const
{
	out.assign(nodes.size(), false);
	xr_vector<int> stack;
	if (from)
	{
		const int i = NodeIndex(from);
		if (i >= 0) { out[i] = true; stack.push_back(i); }
	}
	else
	{
		for (u32 i = 0; i < nodes.size(); ++i)
			if (NqText::IsTrigger(nodes[i].kind.c_str())) { out[i] = true; stack.push_back((int)i); }
	}
	while (!stack.empty())
	{
		const int i = stack.back(); stack.pop_back();
		const SNqNode& n = nodes[i];
		for (u32 p = 0; p < n.out.size(); ++p)
			for (u32 t = 0; t < n.out[p].second.size(); ++t)
			{
				const int j = NodeIndex(n.out[p].second[t].c_str());
				if (j >= 0 && !out[j]) { out[j] = true; stack.push_back(j); }
			}
	}
}

void SNqQuest::DialogChain(LPCSTR topic_id, xr_vector<SDialogHop>& out) const
{
	out.clear();
	const int root = NodeIndex(topic_id);
	if (root < 0) return;
	// visited per parity: a phrase reached at both parities is a real conflict
	// (E010) and has to be reported, so both hops are kept
	xr_vector<u8> seen(nodes.size(), 0);
	xr_vector<SDialogHop> queue;
	SDialogHop h0 = { root, 0 };
	queue.push_back(h0);
	seen[root] |= 1;
	for (u32 q = 0; q < queue.size(); ++q)
	{
		const SDialogHop h = queue[q];
		out.push_back(h);
		const SNqNode& n = nodes[h.node];
		for (u32 p = 0; p < n.out.size(); ++p)
		{
			// only `next` carries phrases; a topic's `done` and a phrase's other
			// pins (none in v1) leave the dialog
			if (n.out[p].first != "next") continue;
			for (u32 t = 0; t < n.out[p].second.size(); ++t)
			{
				const int j = NodeIndex(n.out[p].second[t].c_str());
				if (j < 0 || !NqText::IsPhraseKind(nodes[j].kind.c_str())) continue;
				const int depth = h.depth + 1;
				const u8 bit = (depth & 1) ? 2 : 1;
				if (seen[j] & bit) continue;
				seen[j] |= bit;
				SDialogHop nh = { j, depth };
				queue.push_back(nh);
			}
		}
	}
}

void SNqQuest::TopoOrder(xr_vector<int>& out) const
{
	out.clear();
	xr_vector<bool> done; done.assign(nodes.size(), false);
	// Walk from every trigger in file order. Children of a node are emitted
	// together, in pin order then target order, and then descended into in
	// that order - siblings (the replies of a phrase, the branches of a fork)
	// stay next to each other. Edges that leave a dialog (phrase -> world node)
	// are deferred until the whole dialog has been written, so a conversation
	// reads as one block and the quest continues below it. Same edges = same
	// order, which is what keeps diffs quiet.
	xr_vector<int> stack, deferred;
	for (u32 i = 0; i < nodes.size(); ++i)
	{
		if (!NqText::IsTrigger(nodes[i].kind.c_str()) || done[i]) continue;
		done[i] = true;
		out.push_back((int)i);
		stack.clear();
		deferred.clear();
		stack.push_back((int)i);
		for (;;)
		{
			if (stack.empty())
			{
				if (deferred.empty()) break;
				for (u32 d = 0; d < deferred.size(); ++d) out.push_back(deferred[d]);
				for (int d = (int)deferred.size() - 1; d >= 0; --d) stack.push_back(deferred[d]);
				deferred.clear();
			}
			const int cur = stack.back(); stack.pop_back();
			const SNqNode& n = nodes[cur];
			const bool from_dialog = NqText::IsDialogKind(n.kind.c_str());
			xr_vector<int> kids;
			for (u32 p = 0; p < n.out.size(); ++p)
				for (u32 t = 0; t < n.out[p].second.size(); ++t)
				{
					const int j = NodeIndex(n.out[p].second[t].c_str());
					if (j < 0 || done[j]) continue;
					done[j] = true;
					if (from_dialog && !NqText::IsPhraseKind(nodes[j].kind.c_str())) deferred.push_back(j);
					else kids.push_back(j);
				}
			for (u32 k = 0; k < kids.size(); ++k) out.push_back(kids[k]);
			for (int k = (int)kids.size() - 1; k >= 0; --k) stack.push_back(kids[k]);
		}
	}
	for (u32 i = 0; i < nodes.size(); ++i)
		if (!done[i]) out.push_back((int)i);
}

bool SNqQuest::Equals(const SNqQuest& o) const
{
	if (nq != o.nq || id != o.id || activation != o.activation || !title.Equals(o.title)) return false;
	if (vars.size() != o.vars.size() || tasks.size() != o.tasks.size() || nodes.size() != o.nodes.size()) return false;
	for (u32 i = 0; i < vars.size(); ++i)
		if (vars[i].name != o.vars[i].name || !vars[i].value.Equals(o.vars[i].value)) return false;
	for (u32 i = 0; i < tasks.size(); ++i)
	{
		const SNqTask& a = tasks[i]; const SNqTask& b = o.tasks[i];
		if (a.id != b.id || a.type != b.type || a.icon != b.icon) return false;
		if (!a.title.Equals(b.title) || !a.descr.Equals(b.descr) || !a.target.Equals(b.target)) return false;
	}
	for (u32 i = 0; i < nodes.size(); ++i)
	{
		const SNqNode& a = nodes[i]; const SNqNode& b = o.nodes[i];
		if (a.id != b.id || a.kind != b.kind || a.once != b.once || a.once_set != b.once_set) return false;
		if (!a.params.Equals(b.params) || a.comment != b.comment) return false;
		if (a.has_pos != b.has_pos || (a.has_pos && (a.pos.x != b.pos.x || a.pos.y != b.pos.y))) return false;
		if (a.cond.size() != b.cond.size() || a.on_enter.size() != b.on_enter.size() ||
			a.on_exit.size() != b.on_exit.size() || a.out.size() != b.out.size()) return false;
		for (u32 k = 0; k < a.cond.size(); ++k)
			if (a.cond[k].kind != b.cond[k].kind || a.cond[k].negate != b.cond[k].negate ||
				!a.cond[k].params.Equals(b.cond[k].params)) return false;
		for (u32 k = 0; k < a.on_enter.size(); ++k)
			if (a.on_enter[k].kind != b.on_enter[k].kind || !a.on_enter[k].params.Equals(b.on_enter[k].params)) return false;
		for (u32 k = 0; k < a.on_exit.size(); ++k)
			if (a.on_exit[k].kind != b.on_exit[k].kind || !a.on_exit[k].params.Equals(b.on_exit[k].params)) return false;
		for (u32 k = 0; k < a.out.size(); ++k)
			if (a.out[k].first != b.out[k].first || a.out[k].second != b.out[k].second) return false;
	}
	return true;
}

//------------------------------------------------------------------------------
// NqText
//------------------------------------------------------------------------------
xr_string NqText::Preview(const SNqValue& v)
{
	if (v.IsString()) return v.s;
	if (v.IsTable())
	{
		if (const SNqValue* r = v.Get("rus")) return r->AsString();
		if (const SNqValue* e = v.Get("eng")) return e->AsString();
		if (!v.vals.empty()) return v.vals[0].AsString();
	}
	return v.AsString();
}

bool NqText::ValidId(LPCSTR id)
{
	if (!id || !id[0]) return false;
	for (const char* p = id; *p; ++p)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
	return true;
}

xr_string NqText::RefSummary(const SNqValue& v)
{
	if (!v.IsTable()) return v.AsString();
	xr_string out;
	if (const SNqValue* sp = v.Get("spawn"))
	{
		out = "spawn ";
		out += sp->GetString("section");
		if (sp->Has("smart"))	{ out += "@"; out += sp->GetString("smart"); }
		if (sp->Has("ref"))		{ out += " ref="; out += sp->GetString("ref"); }
		return out;
	}
	static const char* kAlt[] = { "story", "ref", "profile", "smart", "restrictor", "community" };
	for (int i = 0; i < 6; ++i)
		if (v.Has(kAlt[i]))
		{
			out = kAlt[i]; out += ":"; out += v.GetString(kAlt[i]);
			if (0 == strcmp(kAlt[i], "community") && v.Has("level")) { out += "@"; out += v.GetString("level"); }
			return out;
		}
	if (v.Has("level"))
	{
		out = "level:"; out += v.GetString("level");
		if (const SNqValue* pos = v.Get("pos"))
			if (pos->IsTable() && pos->arr.size() >= 3)
			{
				// the three values come from the file and can be strings of any
				// length, so no fixed buffer here
				out += NqUtil::Format(" pos=(%s,%s,%s)", pos->arr[0].AsString().c_str(), pos->arr[1].AsString().c_str(), pos->arr[2].AsString().c_str());
			}
		if (v.Has("radius")) { out += " r="; out += v.GetString("radius"); }
		return out;
	}
	if (v.Has("seconds"))		{ out = v.GetString("seconds");		out += "s";  return out; }
	if (v.Has("game_hours"))	{ out = v.GetString("game_hours");	out += "gh"; return out; }
	if (v.Has("game_minutes"))	{ out = v.GetString("game_minutes");	out += "gm"; return out; }
	return ValueSummary(v);
}

xr_string NqText::ValueSummary(const SNqValue& v, u32 max_len)
{
	xr_string out;
	switch (v.type)
	{
	case SNqValue::tNil:	return "nil";
	case SNqValue::tBool:	return v.b ? "true" : "false";
	case SNqValue::tNumber:	return v.AsString();
	case SNqValue::tString:
		out = "\"";
		out += NqUtil::ClipUtf8(v.s, max_len);
		out += "\"";
		return out;
	case SNqValue::tTable:
		// text tables and refs read better through their dedicated summaries
		if (v.Has("rus") || v.Has("eng")) return ValueSummary(SNqValue::String(Preview(v)), max_len);
		if (v.Has("story") || v.Has("ref") || v.Has("profile") || v.Has("smart") || v.Has("spawn") ||
			v.Has("restrictor") || v.Has("community") || v.Has("level") || v.Has("seconds") ||
			v.Has("game_hours") || v.Has("game_minutes"))
			return RefSummary(v);
		out = "{";
		for (u32 i = 0; i < v.arr.size(); ++i)
		{
			if (i) out += ",";
			// cases: show their names only
			if (v.arr[i].IsTable() && v.arr[i].Has("name")) out += v.arr[i].GetString("name");
			else out += ValueSummary(v.arr[i], 16);
		}
		for (u32 i = 0; i < v.keys.size(); ++i)
		{
			if (i || !v.arr.empty()) out += ",";
			out += v.keys[i]; out += "="; out += ValueSummary(v.vals[i], 16);
		}
		out += "}";
		if (out.size() > max_len + 8) { out = NqUtil::ClipUtf8(out, max_len); out += "}"; }
		return out;
	}
	return out;
}

bool NqText::IsPhraseKind(LPCSTR kind)
{
	return kind && (0 == strcmp(kind, "dialog.npc_phrase") || 0 == strcmp(kind, "dialog.actor_phrase"));
}

bool NqText::IsTopicKind(LPCSTR kind)
{
	return kind && 0 == strcmp(kind, "dialog.topic");
}

bool NqText::IsDialogKind(LPCSTR kind)
{
	return IsTopicKind(kind) || IsPhraseKind(kind);
}

bool NqText::IsTriggerKind(LPCSTR kind)
{
	return kind && 0 == strncmp(kind, "trigger.", 8);
}

bool NqText::IsTrigger(LPCSTR kind)
{
	if (!kind || !kind[0]) return false;
	if (const NqCatalog::SKind* k = NqCatalog::Find(kind))
		return k->Is(NqCatalog::useTrigger) && !k->Is(NqCatalog::useMain);
	return IsTriggerKind(kind);
}
