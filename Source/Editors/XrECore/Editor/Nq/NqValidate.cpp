#include "stdafx.h"
#include "NqValidate.h"
#include "NqCatalog.h"
#include "NqPickers.h"
#include "NqLua.h"
#include "NqUtil.h"

//------------------------------------------------------------------------------
xr_string SNqProblem::Text() const
{
	xr_string s = code;
	if (!node_id.empty()) { s += " "; s += node_id; }
	if (!slot.empty())    { s += "["; s += slot; s += "]"; }
	s += ": ";
	s += message;
	return s;
}

namespace
{
	typedef NqCatalog::SKind	SKind;
	typedef NqCatalog::SParam	SParam;

	struct SRefSite
	{
		xr_string	ref;
		int			node;
		int			order;		// cond: i, on_enter: 100+i, main: 200, on_exit: 300+i
		xr_string	slot;
	};

	struct SCtx
	{
		const SNqQuest&					q;
		const NqValidate::SContext&		ctx;
		xr_vector<SNqProblem>&			out;
		bool							catalog;
		bool							pickers;
		xr_vector<SRefSite>				decls;
		xr_vector<SRefSite>				uses;
		SCtx(const SNqQuest& q_, const NqValidate::SContext& c, xr_vector<SNqProblem>& o)
			: q(q_), ctx(c), out(o), catalog(false), pickers(false) {}
	};

	void Add(SCtx& c, SNqProblem::ESeverity sev, LPCSTR code, LPCSTR node, LPCSTR slot, LPCSTR fmt, ...)
	{
		SNqProblem p;
		p.severity = sev; p.code = code; p.node_id = node ? node : ""; p.slot = slot ? slot : "";
		char buf[1024];
		va_list ap; va_start(ap, fmt);
		_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
		va_end(ap);
		p.message = buf;
		c.out.push_back(p);
	}

	#define ERR(c, code, node, slot, ...)	Add(c, SNqProblem::sevError,   code, node, slot, __VA_ARGS__)
	#define WARN(c, code, node, slot, ...)	Add(c, SNqProblem::sevWarning, code, node, slot, __VA_ARGS__)

	xr_string SlotName(LPCSTR prefix, LPCSTR param)
	{
		xr_string s = prefix ? prefix : "";
		if (param && param[0])
		{
			if (!s.empty()) s += "/";
			s += "param:";
			s += param;
		}
		return s;
	}

	// ---- E022: parameter forms a kind takes only one of -----------------------------
	// The catalog line cannot yet say "these two cancel out", so the shape comes from
	// NqCatalog::ExclusiveForms. The message names the parameters that collided: the
	// inspector shows every one of them at once, so "wrong combination" alone is no help.
	void CheckKindForm(SCtx& c, const xr_string& kind, const SNqValue& params, LPCSTR node)
	{
		xr_vector<xr_string> forms;
		NqCatalog::ExclusiveForms(kind.c_str(), forms);
		if (forms.empty() || !params.IsTable()) return;

		// first parameter present from each form, in the order the file lists them
		xr_vector<xr_string> picked(forms.size());
		for (u32 i = 0; i < params.keys.size(); ++i)
		{
			const int f = NqCatalog::FormIndex(kind.c_str(), params.keys[i].c_str());
			if (f >= 0 && picked[f].empty()) picked[f] = params.keys[i];
		}
		int used = 0, first = -1;
		for (u32 f = 0; f < forms.size(); ++f)
			if (!picked[f].empty()) { ++used; if (first < 0) first = (int)f; }

		if (used > 1)
		{
			xr_string list;
			for (u32 f = 0; f < forms.size(); ++f)
				if (!picked[f].empty()) { if (!list.empty()) list += " and "; list += "'" + picked[f] + "'"; }
			ERR(c, "E022", node, 0, "%s takes only one of these at a time, %s are set together",
				kind.c_str(), list.c_str());
		}
		// objective.fetch has no meaning without a form; the kill_count filters are optional
		if (used == 0 && kind == "objective.fetch")
			ERR(c, "E022", node, 0, "objective.fetch needs either 'section' or 'item'");
	}

	// ---- W060 through the pickers -----------------------------------------------
	void CheckIndexed(SCtx& c, NqPickers::EType t, const xr_string& value, LPCSTR node, LPCSTR slot)
	{
		if (!c.pickers || value.empty() || t == NqPickers::tCount) return;
		if (NqPickers::Index(t).empty()) return;
		if (NqPickers::Known(t, value.c_str())) return;
		// restrictors are the one index the project fills, not the game
		if (t == NqPickers::tRestrictor)
			WARN(c, "W060", node, slot, "'%s' is not a restrictor placed in this project's levels", value.c_str());
		else
			WARN(c, "W060", node, slot, "'%s' is not a known %s of the linked game", value.c_str(), NqPickers::TypeName(t));
	}

	// ---- W040 -----------------------------------------------------------------------
	void CheckText(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot)
	{
		if (v.IsString())
		{
			xr_string bad;
			if (!NqUtil::Cp1251Covers(v.s, bad))
				WARN(c, "W040", node, slot, "text has characters outside cp1251 (%s) - the game will show '?'", bad.c_str());
		}
		else if (v.IsTable())
			for (u32 i = 0; i < v.vals.size(); ++i) CheckText(c, v.vals[i], node, slot);
	}

	bool IsInteger(double d) { return d == floor(d) && fabs(d) < 1e15; }

	// ---- reference shapes ------------------------------------------------------------
	void NoteRefUse(SCtx& c, const SNqValue& tbl, int node, int order, LPCSTR slot)
	{
		if (const SNqValue* r = tbl.Get("ref"))
			if (r->IsString() && !r->s.empty())
			{
				SRefSite s; s.ref = r->s; s.node = node; s.order = order; s.slot = slot ? slot : "";
				c.uses.push_back(s);
			}
	}

	void NoteRefDecl(SCtx& c, const xr_string& name, int node, int order, LPCSTR slot)
	{
		if (name.empty()) return;
		SRefSite s; s.ref = name; s.node = node; s.order = order; s.slot = slot ? slot : "";
		c.decls.push_back(s);
	}

	// npc_ref: { story } | { ref } | { profile } | { community[, level] }
	bool CheckPlace(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot, int nidx, int order);
	void CheckSpawnSpec(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot, int nidx, int order, LPCSTR what);

	bool CheckNpcRef(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot, int nidx, int order, bool allow_smart, bool allow_spawn)
	{
		if (!v.IsTable())
		{
			ERR(c, "E006", node, slot, "expected a reference table { story = ... } / { ref = ... } / { profile = ... } / { community = ... }%s",
				allow_smart ? " / { smart = ... }" : "");
			return false;
		}
		int alts = 0;
		if (v.Has("story"))		{ ++alts; CheckIndexed(c, NqPickers::tStory,     v.GetString("story"), node, slot); }
		if (v.Has("ref"))		{ ++alts; NoteRefUse(c, v, nidx, order, slot); }
		if (v.Has("profile"))	{ ++alts; CheckIndexed(c, NqPickers::tProfile,   v.GetString("profile"), node, slot); }
		if (v.Has("community"))	{ ++alts; CheckIndexed(c, NqPickers::tCommunity, v.GetString("community"), node, slot);
								  if (v.Has("level")) CheckIndexed(c, NqPickers::tLevel, v.GetString("level"), node, slot); }
		if (allow_smart && v.Has("smart")) { ++alts; CheckIndexed(c, NqPickers::tSmart, v.GetString("smart"), node, slot); }
		// a target_ref may also name a place: the runtime anchors a space_restrictor on a
		// { level, pos } target and takes an existing restrictor by name
		// (xms_nq_task.script target_object_id)
		if (allow_smart && v.Has("restrictor"))
		{
			++alts;
			if (!v.Get("restrictor")->IsString()) ERR(c, "E006", node, slot, "restrictor must be a string");
			else CheckIndexed(c, NqPickers::tRestrictor, v.GetString("restrictor"), node, slot);
		}
		if (allow_smart && v.Has("pos"))
		{
			++alts;
			const SNqValue* p = v.Get("pos");
			if (!p->IsTable() || p->arr.size() != 3) ERR(c, "E006", node, slot, "pos must be { x, y, z }");
			if (v.Has("level")) CheckIndexed(c, NqPickers::tLevel, v.GetString("level"), node, slot);
			else				WARN(c, "W060", node, slot, "a { pos } target without a level is placed on the level the quest runs on");
		}
		if (allow_spawn && v.Has("spawn"))
		{
			++alts;
			CheckSpawnSpec(c, *v.Get("spawn"), node, slot, nidx, order, "spawn");
		}
		if (alts == 0)
		{
			ERR(c, "E006", node, slot, "reference table needs one of story/ref/profile/community%s%s",
				allow_smart ? "/smart" : "", allow_spawn ? "/spawn" : "");
			return false;
		}
		if (alts > 1) ERR(c, "E006", node, slot, "reference table must name exactly one target kind");
		for (u32 i = 0; i < v.keys.size(); ++i)
		{
			const xr_string& k = v.keys[i];
			if (k == "level" || k == "pos" || k == "radius") continue;
			if (!v.vals[i].IsString() && k != "spawn")
				ERR(c, "E006", node, slot, "reference field '%s' must be a string", k.c_str());
		}
		return true;
	}

	// squad_ref / object_ref: one concrete world object - { story } | { ref }, nothing else.
	// Narrower than npc_ref on purpose: a profile or a community names a kind of creature,
	// and there is no single object behind that to loot, hold a ref on, or count deaths of.
	void CheckObjectRef(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot, int nidx, int order, LPCSTR type)
	{
		if (!v.IsTable())
		{
			ERR(c, "E006", node, slot, "expected %s = { story = ... } or { ref = ... }", type);
			return;
		}
		int alts = 0;
		if (v.Has("story")) { ++alts; CheckIndexed(c, NqPickers::tStory, v.GetString("story"), node, slot); }
		if (v.Has("ref"))	{ ++alts; NoteRefUse(c, v, nidx, order, slot); }
		if (alts != 1) ERR(c, "E006", node, slot, "%s needs exactly one of story / ref", type);
		for (u32 i = 0; i < v.keys.size(); ++i)
		{
			const xr_string& k = v.keys[i];
			if (k != "story" && k != "ref") ERR(c, "E006", node, slot, "%s has no field '%s'", type, k.c_str());
			else if (!v.vals[i].IsString()) ERR(c, "E006", node, slot, "'%s' must be a string", k.c_str());
		}
	}

	// place: { level, pos = {x,y,z}, radius } | { restrictor } | { smart } | { ref }
	// One spawn spec wherever it appears: the spawn_spec parameter of spawn.squad and the
	// { spawn = ... } form of a kill target. They used to be checked in two places and only one
	// of them looked at the place - which is how a quest could name a restrictor that was never
	// placed, validate clean, and then spawn nobody at all.
	void CheckSpawnSpec(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot, int nidx, int order, LPCSTR what)
	{
		// an empty smart is not a smart: the runtime reads it as unset, so must this
		const xr_string smart = v.GetString("smart");
		if (!v.IsTable() || !v.Has("section") || (smart.empty() && !v.Has("place")))
		{
			ERR(c, "E006", node, slot, "'%s' needs { section = ... } and a smart or a place to put them on", what ? what : "spawn");
			return;
		}
		CheckIndexed(c, NqPickers::tSquad, v.GetString("section"), node, slot);
		CheckIndexed(c, NqPickers::tSmart, smart, node, slot);
		if (const SNqValue* pl = v.Get("place")) CheckPlace(c, *pl, node, slot, nidx, order);
		if (v.Has("restrictor")) CheckIndexed(c, NqPickers::tRestrictor, v.GetString("restrictor"), node, slot);
		NoteRefDecl(c, v.GetString("ref"), nidx, order, slot);
		if (const SNqValue* h = v.Get("hold"))
			if (h->IsBool() && !h->b) WARN(c, "W031", node, slot, "spawned squad without hold - it may leave for the simulation");
	}

	bool CheckPlace(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot, int nidx, int order)
	{
		if (!v.IsTable())
		{
			ERR(c, "E006", node, slot, "expected a place { level = ..., pos = { x, y, z }, radius = r } / { restrictor = ... } / { smart = ... } / { ref = ... }");
			return false;
		}
		// an object the quest creates: nothing to look up in a picker, but it has to
		// be a ref this quest actually declares
		if (v.Has("ref"))
		{
			if (!v.Get("ref")->IsString()) ERR(c, "E006", node, slot, "ref must be a string");
			else NoteRefUse(c, v, nidx, order, slot);
			return true;
		}
		if (v.Has("restrictor"))
		{
			if (!v.Get("restrictor")->IsString()) ERR(c, "E006", node, slot, "restrictor must be a string");
			else CheckIndexed(c, NqPickers::tRestrictor, v.GetString("restrictor"), node, slot);
			return true;
		}
		if (v.Has("smart"))			{ CheckIndexed(c, NqPickers::tSmart, v.GetString("smart"), node, slot); return true; }
		if (v.Has("level") || v.Has("pos"))
		{
			if (!v.Has("level") || !v.Get("level")->IsString()) ERR(c, "E006", node, slot, "place needs level = \"<level name>\"");
			else CheckIndexed(c, NqPickers::tLevel, v.GetString("level"), node, slot);
			const SNqValue* pos = v.Get("pos");
			if (!pos || !pos->IsTable() || pos->arr.size() != 3 || !pos->arr[0].IsNumber() || !pos->arr[1].IsNumber() || !pos->arr[2].IsNumber())
				ERR(c, "E006", node, slot, "place needs pos = { x, y, z }");
			if (const SNqValue* r = v.Get("radius"))
				if (!r->IsNumber() || r->n <= 0) ERR(c, "E006", node, slot, "radius must be a positive number");
			return true;
		}
		ERR(c, "E006", node, slot, "place needs level+pos, restrictor, smart or ref");
		return false;
	}

	bool CheckDuration(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot)
	{
		// the same five forms the runtime resolves (xms_nq_util.duration_secs):
		// a bare number is real seconds
		if (v.IsNumber())
		{
			if (v.n < 0) { ERR(c, "E006", node, slot, "duration must be a non-negative number of seconds"); return false; }
			return true;
		}
		if (!v.IsTable())
		{
			ERR(c, "E006", node, slot, "expected a number of seconds or a duration { seconds = N } / { game_hours = N } / { game_minutes = N } / { game_seconds = N }");
			return false;
		}
		int alts = 0;
		static const char* kUnits[] = { "seconds", "game_hours", "game_minutes", "game_seconds" };
		for (int i = 0; i < 4; ++i)
			if (const SNqValue* u = v.Get(kUnits[i]))
			{
				++alts;
				if (!u->IsNumber() || u->n < 0) ERR(c, "E006", node, slot, "%s must be a non-negative number", kUnits[i]);
			}
		if (alts != 1) { ERR(c, "E006", node, slot, "duration needs exactly one of seconds / game_hours / game_minutes / game_seconds"); return false; }
		return true;
	}

	void CheckCondValue(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot, int nidx, int order, bool events_ok);
	void CheckCases(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot, int nidx, int order, bool weighted, bool events_ok);

	// one parameter against its catalog definition
	// `siblings` is the params table this value came out of: an objective id only
	// means something next to the task it belongs to, and that task is a sibling.
	void CheckParamValue(SCtx& c, const SKind& k, const SParam& p, const SNqValue& v, LPCSTR node, LPCSTR slot,
						 int nidx, int order, bool events_ok, const SNqValue* siblings = 0)
	{
		const xr_string& t = p.type;
		// numbers
		if (t == "int" || t == "float")
		{
			if (!v.IsNumber()) { ERR(c, "E006", node, slot, "'%s' must be a number", p.name.c_str()); return; }
			if (t == "int" && !IsInteger(v.n)) ERR(c, "E006", node, slot, "'%s' must be an integer", p.name.c_str());
			if (p.has_min && v.n < p.min) ERR(c, "E006", node, slot, "'%s' must be >= %g", p.name.c_str(), p.min);
			if (p.has_max && v.n > p.max) ERR(c, "E006", node, slot, "'%s' must be <= %g", p.name.c_str(), p.max);
			return;
		}
		if (t == "bool")
		{
			if (!v.IsBool()) ERR(c, "E006", node, slot, "'%s' must be true or false", p.name.c_str());
			return;
		}
		if (t == "enum" || t == "relation")
		{
			xr_vector<xr_string> allowed = p.enums;
			if (t == "relation" && allowed.empty()) { allowed.push_back("enemy"); allowed.push_back("neutral"); allowed.push_back("friend"); }
			if (!v.IsString()) { ERR(c, "E006", node, slot, "'%s' must be one of the enum values", p.name.c_str()); return; }
			bool ok = allowed.empty();
			for (u32 i = 0; i < allowed.size(); ++i) if (allowed[i] == v.s) ok = true;
			if (!ok)
			{
				xr_string list;
				for (u32 i = 0; i < allowed.size(); ++i) { if (i) list += "|"; list += allowed[i]; }
				ERR(c, "E006", node, slot, "'%s' = \"%s\" is not one of %s", p.name.c_str(), v.s.c_str(), list.c_str());
			}
			return;
		}
		if (t == "text")
		{
			if (!v.IsString() && !(v.IsTable() && !v.keys.empty()))
			{
				ERR(c, "E006", node, slot, "'%s' must be a string or { rus = ..., eng = ... }", p.name.c_str());
				return;
			}
			if (v.IsTable())
				for (u32 i = 0; i < v.vals.size(); ++i)
					if (!v.vals[i].IsString()) { ERR(c, "E006", node, slot, "'%s'.%s must be a string", p.name.c_str(), v.keys[i].c_str()); return; }
			CheckText(c, v, node, slot);
			return;
		}
		if (t == "value")
		{
			if (v.IsTable()) ERR(c, "E006", node, slot, "'%s' must be a bool, number or string", p.name.c_str());
			return;
		}
		if (t == "count_or_all")
		{
			if (v.IsString() && v.s == "all") return;
			if (!v.IsNumber() || !IsInteger(v.n) || v.n < 1) ERR(c, "E006", node, slot, "'%s' must be a positive integer or \"all\"", p.name.c_str());
			return;
		}
		if (t == "lua")
		{
			if (!v.IsString()) { ERR(c, "E006", node, slot, "'%s' must be a string with Lua code", p.name.c_str()); return; }
			NqLua::SError e;
			if (!NqLua::SyntaxCheck(v.s.c_str(), e))
			{
				if (e.message.find("cannot create") != xr_string::npos)
					WARN(c, "W050", node, slot, "Lua syntax not checked: %s", e.message.c_str());
				else if (e.line > 0)
					ERR(c, "E050", node, slot, "Lua syntax error at %d:%d: %s", e.line, e.col, e.message.c_str());
				else
					ERR(c, "E050", node, slot, "Lua syntax error: %s", e.message.c_str());
			}
			return;
		}
		if (t == "duration")	{ CheckDuration(c, v, node, slot); return; }
		if (t == "place")		{ CheckPlace(c, v, node, slot, nidx, order); return; }
		if (t == "npc_ref")		{ CheckNpcRef(c, v, node, slot, nidx, order, false, false); return; }
		if (t == "target_ref")	{ CheckNpcRef(c, v, node, slot, nidx, order, true, false); return; }
		if (t == "kill_target")	{ CheckNpcRef(c, v, node, slot, nidx, order, false, true); return; }
		if (t == "squad_ref" || t == "object_ref") { CheckObjectRef(c, v, node, slot, nidx, order, t.c_str()); return; }
		if (t == "spawn_spec") { CheckSpawnSpec(c, v, node, slot, nidx, order, p.name.c_str()); return; }
		if (t == "cases_cond")		{ CheckCases(c, v, node, slot, nidx, order, false, events_ok); return; }
		if (t == "cases_weight")	{ CheckCases(c, v, node, slot, nidx, order, true, events_ok); return; }
		if (t == "cond_list")
		{
			if (!v.IsTable() || v.arr.empty()) { ERR(c, "E006", node, slot, "'%s' must be a non-empty list of conditions", p.name.c_str()); return; }
			for (u32 i = 0; i < v.arr.size(); ++i)
			{
				xr_string sl = NqUtil::Format("%s/of:%u", slot ? slot : "", i);
				CheckCondValue(c, v.arr[i], node, sl.c_str(), nidx, order, events_ok);
			}
			return;
		}
		// everything else is a plain string: identifiers, sections, names
		if (!v.IsString()) { ERR(c, "E006", node, slot, "'%s' must be a string", p.name.c_str()); return; }
		if (t == "ref_name")
		{
			// a `ref` parameter of a spawning kind declares the name
			if (k.id == "spawn.squad" || k.id == "spawn.object" || k.id == "item.spawn") NoteRefDecl(c, v.s, nidx, order, slot);
			return;
		}
		if (t == "objective_id")
		{
			if (!v.IsString()) { ERR(c, "E006", node, slot, "'%s' must be an objective id", p.name.c_str()); return; }
			// only meaningful next to its task, which every kind taking one passes along
			const SNqValue* owner = siblings ? siblings->Get("task") : 0;
			const SNqTask* task = (owner && owner->IsString()) ? c.q.FindTask(owner->s.c_str()) : 0;
			if (!task) ERR(c, "E030", node, slot, "objective '%s' names no task", v.s.c_str());
			else if (!task->FindObjective(v.s.c_str()))
				ERR(c, "E030", node, slot, "task '%s' declares no objective '%s'", task->id.c_str(), v.s.c_str());
			return;
		}
		if (t == "task_id")
		{
			if (!c.q.FindTask(v.s.c_str())) ERR(c, "E030", node, slot, "task '%s' is not declared in tasks", v.s.c_str());
			return;
		}
		if (t == "var_name")
		{
			if (!c.q.FindVar(v.s.c_str())) ERR(c, "E030", node, slot, "variable '%s' is not declared in vars", v.s.c_str());
			return;
		}
		if (t == "node_id")
		{
			if (!c.q.FindNode(v.s.c_str())) ERR(c, "E030", node, slot, "node '%s' does not exist", v.s.c_str());
			return;
		}
		if (t == "quest_id")
		{
			// "<module>.<quest>" cannot be checked here; a bare id must be a quest of this project
			if (v.s.find('.') == xr_string::npos && c.ctx.project_quest_ids)
			{
				bool known = (v.s == c.q.id);
				for (u32 i = 0; i < c.ctx.project_quest_ids->size() && !known; ++i)
					known = ((*c.ctx.project_quest_ids)[i] == v.s);
				if (!known) ERR(c, "E030", node, slot, "quest '%s' is not a quest of this project", v.s.c_str());
			}
			return;
		}
		const NqPickers::EType pt = NqPickers::TypeFromName(t.c_str());
		if (pt != NqPickers::tCount) CheckIndexed(c, pt, v.s, node, slot);
	}

	// all params of an entity (node main / action / cond) against the kind
	void CheckParams(SCtx& c, const SKind& k, const SNqValue& params, LPCSTR node, LPCSTR slot_prefix,
					 int nidx, int order, bool events_ok)
	{
		if (!params.IsNil() && !params.IsTable())
		{
			ERR(c, "E006", node, slot_prefix, "params must be a table");
			return;
		}
		for (u32 i = 0; i < k.params.size(); ++i)
		{
			const SParam& p = k.params[i];
			const SNqValue* v = params.Get(p.name.c_str());
			xr_string slot = SlotName(slot_prefix, p.name.c_str());
			if (!v || v->IsNil())
			{
				if (p.required) ERR(c, "E006", node, slot.c_str(), "required parameter '%s' (%s) is missing", p.name.c_str(), p.type.c_str());
				continue;
			}
			CheckParamValue(c, k, p, *v, node, slot.c_str(), nidx, order, events_ok, &params);
		}
	}

	void CheckCond(SCtx& c, const SNqCond& cd, LPCSTR node, LPCSTR slot, int nidx, int order, bool events_ok)
	{
		const SKind* k = NqCatalog::Find(cd.kind.c_str());
		if (!k || !k->Is(NqCatalog::useCond))
		{
			if (c.catalog) ERR(c, "E005", node, slot, "'%s' is not a condition kind", cd.kind.c_str());
			return;
		}
		if (k->event && !events_ok)
			ERR(c, "E020", node, slot, "event condition '%s' is only allowed in trigger.when / wait.when / wait.any", cd.kind.c_str());
		CheckParams(c, *k, cd.params, node, slot, nidx, order, events_ok);
	}

	void CheckCondValue(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot, int nidx, int order, bool events_ok)
	{
		SNqCond cd; xr_string err;
		if (!NqLua::CondFromValue(v, cd, err)) { ERR(c, "E006", node, slot, "%s", err.c_str()); return; }
		CheckCond(c, cd, node, slot, nidx, order, events_ok);
	}

	void CheckCases(SCtx& c, const SNqValue& v, LPCSTR node, LPCSTR slot, int nidx, int order, bool weighted, bool events_ok)
	{
		if (!v.IsTable() || v.arr.empty()) { ERR(c, "E021", node, slot, "cases must be a non-empty list { { name = ..., %s }, ... }", weighted ? "weight = N" : "cond = { ... }"); return; }
		xr_vector<xr_string> names;
		for (u32 i = 0; i < v.arr.size(); ++i)
		{
			xr_string sl = NqUtil::Format("%s%scase:%u", slot ? slot : "", (slot && slot[0]) ? "/" : "", i);
			const SNqValue& cs = v.arr[i];
			if (!cs.IsTable()) { ERR(c, "E021", node, sl.c_str(), "case must be a table"); continue; }
			xr_string name = cs.GetString("name");
			if (!NqText::ValidId(name.c_str())) ERR(c, "E021", node, sl.c_str(), "case name '%s' must be [a-z0-9_]+", name.c_str());
			for (u32 j = 0; j < names.size(); ++j) if (names[j] == name) ERR(c, "E021", node, sl.c_str(), "duplicate case name '%s'", name.c_str());
			names.push_back(name);
			if (weighted)
			{
				const SNqValue* w = cs.Get("weight");
				if (!w || !w->IsNumber() || w->n <= 0) ERR(c, "E021", node, sl.c_str(), "case '%s' needs weight > 0", name.c_str());
			}
			else
			{
				const SNqValue* cl = cs.Get("cond");
				if (!cl || !cl->IsTable()) { ERR(c, "E021", node, sl.c_str(), "case '%s' needs cond = { { kind = ... }, ... }", name.c_str()); continue; }
				if (cl->arr.empty() && cl->Has("kind")) CheckCondValue(c, *cl, node, sl.c_str(), nidx, order, events_ok);
				for (u32 j = 0; j < cl->arr.size(); ++j)
				{
					xr_string sl2 = NqUtil::Format("%s/cond:%u", sl.c_str(), j);
					CheckCondValue(c, cl->arr[j], node, sl2.c_str(), nidx, order, events_ok);
				}
			}
		}
	}

	void CheckAction(SCtx& c, const SNqAction& a, LPCSTR node, LPCSTR slot, int nidx, int order)
	{
		const SKind* k = NqCatalog::Find(a.kind.c_str());
		if (!k || !k->Is(NqCatalog::useExtra))
		{
			if (c.catalog) ERR(c, "E005", node, slot, "'%s' is not an extra-action kind", a.kind.c_str());
			return;
		}
		CheckParams(c, *k, a.params, node, slot, nidx, order, false);
		if (k->id == "spawn.squad")
			if (const SNqValue* h = a.params.Get("hold"))
				if (h->IsBool() && !h->b) WARN(c, "W031", node, slot, "spawn.squad without hold - the squad may leave for the simulation");
		if (k->id == "actor.teleport" && c.ctx.level && c.ctx.level[0])
		{
			const SNqValue* place = a.params.Get("place");
			if (place && place->IsTable() && place->Has("level"))
			{
				const xr_string lv = place->GetString("level");
				if (!lv.empty() && 0 != _stricmp(lv.c_str(), c.ctx.level))
					ERR(c, "E031", node, slot, "actor.teleport to level '%s' - the actor is on '%s'; there is no cross-level teleport", lv.c_str(), c.ctx.level);
			}
		}
	}

	// case names of a node's `cases` param (pins of wait.any / flow.branch / flow.random)
	void CaseNames(const SNqNode& n, xr_vector<xr_string>& out)
	{
		out.clear();
		const SNqValue* cases = n.params.Get("cases");
		if (!cases || !cases->IsTable()) return;
		for (u32 i = 0; i < cases->arr.size(); ++i)
		{
			xr_string name = cases->arr[i].GetString("name");
			if (!name.empty()) out.push_back(name);
		}
	}

	bool PinDeclared(const SKind& k, const SNqNode& n, const xr_string& pin)
	{
		if (k.HasPin(pin.c_str())) return true;
		if (k.pins_from_cases)
		{
			xr_vector<xr_string> names;
			CaseNames(n, names);
			for (u32 i = 0; i < names.size(); ++i) if (names[i] == pin) return true;
		}
		return false;
	}

	// ---- graph rules -----------------------------------------------------------------
	void CheckDialogs(SCtx& c)
	{
		const SNqQuest& q = c.q;
		xr_vector<bool> in_chain; in_chain.assign(q.nodes.size(), false);
		for (u32 t = 0; t < q.nodes.size(); ++t)
		{
			const SNqNode& topic = q.nodes[t];
			if (!NqText::IsTopicKind(topic.kind.c_str())) continue;
			const bool actor_first = topic.params.GetString("initiator", "actor") != "npc";
			xr_vector<SNqQuest::SDialogHop> hops;
			q.DialogChain(topic.id.c_str(), hops);
			// per node: parity seen (bit1 = even, bit2 = odd)
			xr_vector<u8> parity(q.nodes.size(), 0);
			for (u32 h = 0; h < hops.size(); ++h)
			{
				const int ni = hops[h].node;
				in_chain[ni] = true;
				if (hops[h].depth == 0) continue;
				parity[ni] |= (hops[h].depth & 1) ? 2 : 1;
			}
			for (u32 ni = 0; ni < q.nodes.size(); ++ni)
			{
				if (!parity[ni]) continue;
				const SNqNode& ph = q.nodes[ni];
				// odd depth = the other side; even = the initiator's side
				const bool is_actor = (ph.kind == "dialog.actor_phrase");
				const u8 want = (is_actor == actor_first) ? 1 : 2;		// initiator side speaks at even depth
				if (parity[ni] & ~want)
					ERR(c, "E010", ph.id.c_str(), 0, "%s at the wrong depth in dialog '%s' - speakers must alternate (%s speaks at %s depth here)",
						is_actor ? "actor phrase" : "npc phrase", topic.id.c_str(),
						is_actor ? "the actor" : "the npc", (want == 1) ? "even" : "odd");
			}
			// W012: topic without phrases
			bool has_phrase = false;
			if (const xr_vector<xr_string>* nx = topic.Targets("next"))
				for (u32 i = 0; i < nx->size(); ++i)
					if (const SNqNode* tn = q.FindNode((*nx)[i].c_str()))
						if (NqText::IsPhraseKind(tn->kind.c_str())) has_phrase = true;
			if (!has_phrase) WARN(c, "W012", topic.id.c_str(), 0, "dialog topic has no phrase after it (out.next)");
			// W013: repeatable topic without conditions
			if (topic.once_set && !topic.once && topic.cond.empty())
				WARN(c, "W013", topic.id.c_str(), 0, "topic with once=false and no cond will be offered forever");
		}
		// W011: a non-leaf PHRASE needs an unconditional continuation. A topic is
		// not covered (par. 15 and the runtime walk both start at its phrases);
		// a topic without phrases is W012 above.
		for (u32 i = 0; i < q.nodes.size(); ++i)
		{
			const SNqNode& n = q.nodes[i];
			if (!NqText::IsPhraseKind(n.kind.c_str())) continue;
			const xr_vector<xr_string>* nx = n.Targets("next");
			if (!nx) continue;
			int phrases = 0, unconditional = 0;
			bool to_actor = false;
			for (u32 k = 0; k < nx->size(); ++k)
			{
				const SNqNode* tn = q.FindNode((*nx)[k].c_str());
				if (!tn || !NqText::IsPhraseKind(tn->kind.c_str())) continue;
				++phrases;
				if (tn->kind == "dialog.actor_phrase") to_actor = true;
				// the runtime counts `once` as a precondition too - a phrase that
				// already fired is filtered exactly like a failed cond
				if (tn->cond.empty() && !(tn->once_set && tn->once)) ++unconditional;
			}
			// The runtime injects a hidden leaf whenever the children are not ALL
			// unconditional, and that leaf ends the talk without passing the topic:
			// no on_exit, no `done`. Among NPC replies it sits last and one
			// unconditional sibling always outranks it, so it is unreachable. In a
			// player's option list every survivor is shown, so it becomes a real
			// way out that silently skips whatever hangs off `done`.
			if (phrases > 0 && (to_actor ? unconditional < phrases : unconditional == 0))
				WARN(c, "W011", n.id.c_str(), "out:next", to_actor
					? "conditional player options - the game adds a hidden way out that does not fire the topic's done pin"
					: "no unconditional continuation - the game adds an automatic reply when every phrase is filtered");
		}
		// E010 (unreachable from a topic) / E011 (entered from the world)
		for (u32 i = 0; i < q.nodes.size(); ++i)
		{
			const SNqNode& n = q.nodes[i];
			if (!NqText::IsPhraseKind(n.kind.c_str())) continue;
			if (!in_chain[i]) ERR(c, "E010", n.id.c_str(), 0, "phrase is not reachable from any dialog.topic through next pins");
			for (u32 s = 0; s < q.nodes.size(); ++s)
			{
				const SNqNode& src = q.nodes[s];
				for (u32 p = 0; p < src.out.size(); ++p)
					for (u32 t = 0; t < src.out[p].second.size(); ++t)
					{
						if (src.out[p].second[t] != n.id) continue;
						// the pin does not matter here, only the source kind (the
						// runtime discards it too); a phrase no `next` edge reaches
						// is already covered by E010 above
						if (!NqText::IsDialogKind(src.kind.c_str()))
							ERR(c, "E011", n.id.c_str(), 0, "phrase is entered from '%s' (%s.%s) - phrases may only follow a topic or another phrase",
								src.id.c_str(), src.kind.c_str(), src.out[p].first.c_str());
					}
			}
		}
	}

	// W030: a ref used where a path from a trigger reaches the use without a
	// declaring node; E030 when nothing declares it at all
	void CheckRefs(SCtx& c)
	{
		const SNqQuest& q = c.q;
		for (u32 u = 0; u < c.uses.size(); ++u)
		{
			const SRefSite& use = c.uses[u];
			LPCSTR node = (use.node >= 0 && use.node < (int)q.nodes.size()) ? q.nodes[use.node].id.c_str() : 0;
			xr_vector<int> declaring;
			bool declared_here_before = false;
			for (u32 d = 0; d < c.decls.size(); ++d)
			{
				if (c.decls[d].ref != use.ref) continue;
				declaring.push_back(c.decls[d].node);
				if (c.decls[d].node == use.node && c.decls[d].order <= use.order) declared_here_before = true;
			}
			if (declaring.empty()) { ERR(c, "E030", node, use.slot.c_str(), "ref '%s' is never created by this quest", use.ref.c_str()); continue; }
			if (declared_here_before || use.node < 0) continue;
			// reachability from triggers avoiding declaring nodes (other than the using node itself)
			xr_vector<bool> seen; seen.assign(q.nodes.size(), false);
			xr_vector<int> stack;
			for (u32 i = 0; i < q.nodes.size(); ++i)
				if (NqText::IsTrigger(q.nodes[i].kind.c_str())) { seen[i] = true; stack.push_back((int)i); }
			bool reached = false;
			while (!stack.empty() && !reached)
			{
				const int cur = stack.back(); stack.pop_back();
				bool blocks = false;
				for (u32 d = 0; d < declaring.size(); ++d) if (declaring[d] == cur && cur != use.node) blocks = true;
				if (cur == use.node) { reached = true; break; }
				if (blocks) continue;
				const SNqNode& n = q.nodes[cur];
				for (u32 p = 0; p < n.out.size(); ++p)
					for (u32 t = 0; t < n.out[p].second.size(); ++t)
					{
						const int j = q.NodeIndex(n.out[p].second[t].c_str());
						if (j >= 0 && !seen[j]) { seen[j] = true; stack.push_back(j); }
					}
			}
			if (reached)
				WARN(c, "W030", node, use.slot.c_str(), "ref '%s' may be used before the node that creates it (not on every path)", use.ref.c_str());
		}
	}
}

//------------------------------------------------------------------------------
void NqValidate::Run(const SNqQuest& q, const SContext& ctx, xr_vector<SNqProblem>& out)
{
	SCtx c(q, ctx, out);
	c.catalog = NqCatalog::Ensure();
	c.pickers = ctx.use_pickers && NqPickers::Available();

	// ---- quest level ---------------------------------------------------------------
	if (q.nq != 1)
	{
		if (q.nq == 0)	ERR(c, "E001", 0, 0, "nq (format version) is missing - write nq = 1");
		else			ERR(c, "E001", 0, 0, "nq = %d is not supported (this editor writes nq = 1)", q.nq);
	}
	if (!NqText::ValidId(q.id.c_str()))
		ERR(c, "E002", 0, 0, "quest id '%s' must be [a-z0-9_]+", q.id.c_str());
	else if (ctx.project_quest_ids)
	{
		int dups = 0;
		for (u32 i = 0; i < ctx.project_quest_ids->size(); ++i) if ((*ctx.project_quest_ids)[i] == q.id) ++dups;
		if (dups) ERR(c, "E002", 0, 0, "quest id '%s' is used by another quest of this project", q.id.c_str());
	}
	if (q.activation != "auto" && q.activation != "manual")
		ERR(c, "E006", 0, "param:activation", "activation must be \"auto\" or \"manual\"");
	// an absent title is legal - the runtime falls back to the quest id; only a
	// title that IS there and is not text is an error
	if (!q.title.IsNil())
	{
		CheckText(c, q.title, 0, "param:title");
		if (!q.title.IsString() && !(q.title.IsTable() && !q.title.keys.empty()))
			ERR(c, "E006", 0, "param:title", "title must be a string or { rus = ..., eng = ... }");
	}
	for (u32 i = 0; i < q.vars.size(); ++i)
	{
		if (!NqText::ValidId(q.vars[i].name.c_str()))
			ERR(c, "E006", 0, "param:vars", "variable name '%s' must be [a-z0-9_]+", q.vars[i].name.c_str());
		if (q.vars[i].value.IsTable() || q.vars[i].value.IsNil())
			ERR(c, "E006", 0, "param:vars", "variable '%s' default must be a bool, number or string", q.vars[i].name.c_str());
	}
	for (u32 i = 0; i < q.tasks.size(); ++i)
	{
		const SNqTask& t = q.tasks[i];
		xr_string sl = NqUtil::Format("task:%s", t.id.c_str());
		if (!NqText::ValidId(t.id.c_str())) ERR(c, "E006", 0, sl.c_str(), "task id '%s' must be [a-z0-9_]+", t.id.c_str());
		// same as the quest title: absent is legal (the task id is used instead)
		if (!t.title.IsNil()) CheckText(c, t.title, 0, sl.c_str());
		if (!t.descr.IsNil()) CheckText(c, t.descr, 0, sl.c_str());
		if (t.type != "additional" && t.type != "storyline") ERR(c, "E006", 0, sl.c_str(), "task type must be \"additional\" or \"storyline\"");
		if (!t.target.IsNil()) CheckNpcRef(c, t.target, 0, sl.c_str(), -1, 0, true, false);
		for (u32 j = 0; j < t.objectives.size(); ++j)
		{
			const SNqObjective& o = t.objectives[j];
			xr_string osl = NqUtil::Format("task:%s/objective:%s", t.id.c_str(), o.id.c_str());
			if (!NqText::ValidId(o.id.c_str())) ERR(c, "E006", 0, osl.c_str(), "objective id '%s' must be [a-z0-9_]+", o.id.c_str());
			for (u32 k = 0; k < j; ++k)
				if (t.objectives[k].id == o.id) ERR(c, "E006", 0, osl.c_str(), "duplicate objective id '%s'", o.id.c_str());
			if (!o.title.IsNil()) CheckText(c, o.title, 0, osl.c_str());
			if (!o.descr.IsNil()) CheckText(c, o.descr, 0, osl.c_str());
			if (!o.target.IsNil()) CheckNpcRef(c, o.target, 0, osl.c_str(), -1, 0, true, false);
		}
	}
	if (ctx.file_path && ctx.file_path[0])
	{
		xr_string base = NqUtil::BaseName(ctx.file_path);
		if (!q.id.empty() && base != q.id)
			WARN(c, "W061", 0, 0, "file name '%s' differs from quest id '%s'", base.c_str(), q.id.c_str());
	}

	// ---- nodes -----------------------------------------------------------------------
	int triggers = 0, ends = 0;
	for (u32 i = 0; i < q.nodes.size(); ++i)
	{
		const SNqNode& n = q.nodes[i];
		LPCSTR nid = n.id.c_str();
		if (!NqText::ValidId(nid)) ERR(c, "E004", nid, 0, "node id '%s' must be [a-z0-9_]+", nid);
		for (u32 j = 0; j < i; ++j) if (q.nodes[j].id == n.id) { ERR(c, "E004", nid, 0, "duplicate node id '%s'", nid); break; }

		const SKind* k = NqCatalog::Find(n.kind.c_str());
		const bool is_trigger = NqText::IsTrigger(n.kind.c_str());
		if (is_trigger) ++triggers;
		if (n.kind == "flow.end") ++ends;
		if (!k || !(k->Is(NqCatalog::useMain) || k->Is(NqCatalog::useTrigger)))
		{
			if (c.catalog) ERR(c, "E005", nid, 0, "'%s' is not a node kind (main action or trigger)", n.kind.c_str());
		}
		else
		{
			// event conditions are only for the edge-triggered kinds
			const bool events_ok = (n.kind == "trigger.when" || n.kind == "wait.when" || n.kind == "wait.any");
			CheckParams(c, *k, n.params, nid, "", (int)i, 200, events_ok);
			CheckKindForm(c, n.kind, n.params, nid);
			// pins
			for (u32 p = 0; p < n.out.size(); ++p)
			{
				const xr_string& pin = n.out[p].first;
				xr_string sl = NqUtil::Format("out:%s", pin.c_str());
				if (!PinDeclared(*k, n, pin))
					ERR(c, "E007", nid, sl.c_str(), "pin '%s' is not declared by %s", pin.c_str(), n.kind.c_str());
				xr_flat_hash_map<xr_string, u8> seen_targets;
				for (u32 t = 0; t < n.out[p].second.size(); ++t)
				{
					const xr_string& target = n.out[p].second[t];
					if (!seen_targets.emplace(target, u8(1)).second)
					{
						WARN(c, "W022", nid, sl.c_str(), "target '%s' is listed twice, the duplicate edge is ignored", target.c_str());
						continue;
					}
					const SNqNode* tn = q.FindNode(target.c_str());
					if (!tn) { ERR(c, "E007", nid, sl.c_str(), "target node '%s' does not exist", target.c_str()); continue; }
					if (NqText::IsTrigger(tn->kind.c_str()))
						ERR(c, "E007", nid, sl.c_str(), "edge leads into trigger '%s' - triggers have no input", tn->id.c_str());
				}
			}
			if (n.kind == "flow.join" && q.InDegree(nid) == 0)
				ERR(c, "E009", nid, 0, "flow.join has no incoming edges");
			if (k->wait && n.OutDegree() == 0 && !NqText::IsTopicKind(n.kind.c_str()))
				WARN(c, "W021", nid, 0, "waiting node has no outgoing edges - the quest will stop here");
			// conditions on the node
			for (u32 ci = 0; ci < n.cond.size(); ++ci)
			{
				char sl[32]; sprintf_s(sl, "cond:%u", ci);
				CheckCond(c, n.cond[ci], nid, sl, (int)i, (int)ci, events_ok);
			}
		}
		for (u32 a = 0; a < n.on_enter.size(); ++a)
		{
			char sl[32]; sprintf_s(sl, "enter:%u", a);
			CheckAction(c, n.on_enter[a], nid, sl, (int)i, 100 + (int)a);
		}
		for (u32 a = 0; a < n.on_exit.size(); ++a)
		{
			char sl[32]; sprintf_s(sl, "exit:%u", a);
			CheckAction(c, n.on_exit[a], nid, sl, (int)i, 300 + (int)a);
		}
		if (!n.comment.empty()) CheckText(c, SNqValue::String(n.comment), nid, "comment");
	}
	if (triggers == 0)	ERR(c, "E008", 0, 0, "the quest has no trigger node (trigger.start / trigger.when)");
	if (ends == 0)		WARN(c, "W070", 0, 0, "no flow.end - the quest completes implicitly when nothing is left to do");

	// W020 unreachable
	{
		xr_vector<bool> reach;
		q.Reachable(reach);
		for (u32 i = 0; i < q.nodes.size(); ++i)
			if (!reach[i] && !NqText::IsTrigger(q.nodes[i].kind.c_str()))
				WARN(c, "W020", q.nodes[i].id.c_str(), 0, "node is not reachable from any trigger");
	}

	CheckDialogs(c);
	CheckRefs(c);
}

void NqValidate::ShapeErrors(const xr_vector<xr_string>& shape, xr_vector<SNqProblem>& out)
{
	for (u32 i = 0; i < shape.size(); ++i)
	{
		SNqProblem p;
		p.severity = SNqProblem::sevError;
		p.code = "E003";
		p.message = shape[i];
		out.push_back(p);
	}
}

int NqValidate::CountErrors(const xr_vector<SNqProblem>& v)
{
	int n = 0;
	for (u32 i = 0; i < v.size(); ++i) if (v[i].IsError()) ++n;
	return n;
}

int NqValidate::CountWarnings(const xr_vector<SNqProblem>& v)
{
	int n = 0;
	for (u32 i = 0; i < v.size(); ++i) if (!v[i].IsError()) ++n;
	return n;
}

void NqValidate::ToJson(const xr_vector<SNqProblem>& v, xr_string& out)
{
	out += "[";
	for (u32 i = 0; i < v.size(); ++i)
	{
		const SNqProblem& p = v[i];
		if (i) out += ",";
		out += "{\"code\":";		NqUtil::JsonString(out, p.code);
		out += ",\"severity\":";	out += p.IsError() ? "\"error\"" : "\"warning\"";
		out += ",\"node\":";		NqUtil::JsonString(out, p.node_id);
		out += ",\"slot\":";		NqUtil::JsonString(out, p.slot);
		out += ",\"message\":";		NqUtil::JsonString(out, p.message);
		out += "}";
	}
	out += "]";
}

xr_string NqValidate::ToText(const xr_vector<SNqProblem>& v)
{
	if (v.empty()) return "problems: none";
	xr_string s = "problems:";
	for (u32 i = 0; i < v.size(); ++i) { s += "\n"; s += v[i].Text(); }
	return s;
}

void NqValidate::DeclaredRefs(const SNqQuest& q, xr_vector<xr_string>& out)
{
	out.clear();
	xr_vector<SNqProblem> sink;
	SContext ctx;
	ctx.use_pickers = false;
	SCtx c(q, ctx, sink);
	c.catalog = NqCatalog::Ensure();
	c.pickers = false;
	for (u32 i = 0; i < q.nodes.size(); ++i)
	{
		const SNqNode& n = q.nodes[i];
		if (const SKind* k = NqCatalog::Find(n.kind.c_str())) CheckParams(c, *k, n.params, n.id.c_str(), "", (int)i, 200, true);
		for (u32 a = 0; a < n.on_enter.size(); ++a)
			if (const SKind* k = NqCatalog::Find(n.on_enter[a].kind.c_str())) CheckParams(c, *k, n.on_enter[a].params, n.id.c_str(), "", (int)i, 100 + a, false);
		for (u32 a = 0; a < n.on_exit.size(); ++a)
			if (const SKind* k = NqCatalog::Find(n.on_exit[a].kind.c_str())) CheckParams(c, *k, n.on_exit[a].params, n.id.c_str(), "", (int)i, 300 + a, false);
	}
	for (u32 d = 0; d < c.decls.size(); ++d)
	{
		bool dup = false;
		for (u32 k = 0; k < out.size(); ++k) if (out[k] == c.decls[d].ref) dup = true;
		if (!dup) out.push_back(c.decls[d].ref);
	}
}
