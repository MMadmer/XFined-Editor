#include "stdafx.h"
#include "NqReferences.h"
#include "NqCatalog.h"
#include "NqDoc.h"
#include "NqLua.h"
#include "NqUtil.h"

namespace
{
	bool IsKnownType(const xr_string& type)
	{
		static const char* types[] =
		{
			"string", "int", "float", "bool", "enum", "text", "duration", "npc_ref", "target_ref",
			"place", "spawn_spec", "item_section", "squad_section", "level", "smart", "story_id",
			"profile", "community", "info", "var_name", "task_id", "quest_id", "ref_name",
			"signal_name", "spot_type", "relation", "lua", "cases", "cases_cond", "cases_weight",
			"kill_target", "count_or_all", "value", "cond_list", "node_id",
		};
		for (u32 i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
			if (type == types[i]) return true;
		return false;
	}

	bool IsStringType(const xr_string& type)
	{
		return type == "string" || type == "enum" || type == "item_section" || type == "squad_section"
			|| type == "level" || type == "smart" || type == "story_id" || type == "profile"
			|| type == "community" || type == "info" || type == "var_name" || type == "task_id"
			|| type == "quest_id" || type == "ref_name" || type == "signal_name" || type == "spot_type"
			|| type == "relation" || type == "cases" || type == "node_id";
	}

	bool IsInteger(double value)
	{
		return value == floor(value) && fabs(value) < 1e15;
	}

	class SWalker
	{
	public:
		SWalker(LPCSTR path, LPCSTR source, LPCSTR type, LPCSTR value,
			xr_vector<NqReferences::SReference>& references,
			xr_vector<NqReferences::SDiagnostic>& diagnostics,
			xr_vector<SNqValue*>* edits = 0)
			: m_Path(path ? path : ""), m_Source(source ? source : "disk"), m_Type(type ? type : ""),
			  m_Value(value ? value : ""), m_References(references), m_Diagnostics(diagnostics), m_Edits(edits)
		{
		}

		void Quest(SNqQuest& quest)
		{
			// Task targets have a fixed format schema rather than a catalog kind;
			// these descriptors only provide metadata to the shared typed walker.
			NqCatalog::SKind task_kind;
			task_kind.id = "quest.task";
			NqCatalog::SParam target_param;
			target_param.name = "target";
			target_param.type = "target_ref";
			for (u32 i = 0; i < quest.tasks.size(); ++i)
				if (!quest.tasks[i].target.IsNil())
					WalkNpcRef(task_kind, target_param, quest.tasks[i].target, "",
						NqUtil::Format("task:%s/target", quest.tasks[i].id.c_str()).c_str(), true, false);

			for (u32 i = 0; i < quest.nodes.size(); ++i)
			{
				SNqNode& node = quest.nodes[i];
				WalkKind(node.kind.c_str(), node.params, node.id.c_str(), "params",
					NqCatalog::useTrigger | NqCatalog::useMain);
				for (u32 c = 0; c < node.cond.size(); ++c)
				{
					SNqCond& condition = node.cond[c];
					WalkKind(condition.kind.c_str(), condition.params, node.id.c_str(),
						NqUtil::Format("cond:%u", c).c_str(), NqCatalog::useCond);
				}
				WalkActions(node.on_enter, node.id.c_str(), "enter");
				WalkActions(node.on_exit, node.id.c_str(), "exit");
			}
		}

	private:
		xr_string	m_Path;
		xr_string	m_Source;
		xr_string	m_Type;
		xr_string	m_Value;
		xr_vector<NqReferences::SReference>& m_References;
		xr_vector<NqReferences::SDiagnostic>& m_Diagnostics;
		xr_vector<SNqValue*>* m_Edits;

		void Diagnostic(LPCSTR code, LPCSTR message, LPCSTR node, LPCSTR slot,
			LPCSTR kind = "", LPCSTR param = "")
		{
			NqReferences::SDiagnostic diagnostic;
			diagnostic.path = m_Path;
			diagnostic.node = node ? node : "";
			diagnostic.slot = slot ? slot : "";
			diagnostic.kind = kind ? kind : "";
			diagnostic.param = param ? param : "";
			diagnostic.code = code;
			diagnostic.message = message;
			diagnostic.source = m_Source;
			m_Diagnostics.push_back(diagnostic);
		}

		void WalkActions(xr_vector<SNqAction>& actions, LPCSTR node, LPCSTR prefix)
		{
			for (u32 i = 0; i < actions.size(); ++i)
				WalkKind(actions[i].kind.c_str(), actions[i].params, node,
					NqUtil::Format("%s:%u", prefix, i).c_str(), NqCatalog::useExtra);
		}

		void WalkKind(LPCSTR kind_id, SNqValue& params, LPCSTR node, LPCSTR slot, u32 expected_use)
		{
			const NqCatalog::SKind* kind = NqCatalog::Find(kind_id);
			if (!kind)
			{
				Diagnostic("unknown_kind", NqUtil::Format("kind '%s' is absent from the active catalog", kind_id).c_str(),
					node, slot, kind_id);
				return;
			}
			if (!(kind->use & expected_use))
			{
				Diagnostic("wrong_kind_use", NqUtil::Format("kind '%s' is not valid in this slot", kind_id).c_str(),
					node, slot, kind_id);
				return;
			}
			if (!params.IsNil() && !params.IsTable())
			{
				Diagnostic("malformed", "params must be a table", node, slot, kind_id);
				return;
			}
			if (params.IsTable() && !params.arr.empty())
				Diagnostic("malformed", "params must not contain array entries", node, slot, kind_id);

			for (u32 i = 0; i < params.keys.size(); ++i)
				if (!kind->Param(params.keys[i].c_str()))
					Diagnostic("unknown_param",
						NqUtil::Format("parameter '%s' is absent from kind '%s' in the active catalog",
							params.keys[i].c_str(), kind_id).c_str(), node,
						NqUtil::Format("%s/param:%s", slot, params.keys[i].c_str()).c_str(), kind_id,
						params.keys[i].c_str());

			for (u32 i = 0; i < kind->params.size(); ++i)
			{
				const NqCatalog::SParam& param = kind->params[i];
				const xr_string param_slot = NqUtil::Format("%s/param:%s", slot, param.name.c_str());
				SNqValue* value = params.Get(param.name.c_str());
				if (!value || value->IsNil())
				{
					if (param.required)
						Diagnostic("malformed", NqUtil::Format("required parameter '%s' (%s) is missing",
							param.name.c_str(), param.type.c_str()).c_str(), node, param_slot.c_str(), kind_id,
							param.name.c_str());
					continue;
				}
				WalkParam(*kind, param, *value, node, param_slot.c_str());
			}
		}

		void Reference(const NqCatalog::SKind& kind, const NqCatalog::SParam& param,
			LPCSTR type, SNqValue& value, LPCSTR node, LPCSTR slot)
		{
			if (m_Type != type || value.s != m_Value) return;
			NqReferences::SReference reference;
			reference.path = m_Path;
			reference.node = node ? node : "";
			reference.slot = slot ? slot : "";
			reference.kind = kind.id;
			reference.param = param.name;
			reference.type = type;
			reference.value = value.s;
			reference.source = m_Source;
			m_References.push_back(reference);
			if (m_Edits) m_Edits->push_back(&value);
		}

		bool WalkObject(const NqCatalog::SKind& kind, const NqCatalog::SParam& param,
			SNqValue& value, LPCSTR node, LPCSTR slot, LPCSTR schema, const LPCSTR* allowed, u32 allowed_count)
		{
			if (!value.IsTable())
			{
				Diagnostic("malformed", NqUtil::Format("%s must be a table", schema).c_str(), node, slot,
					kind.id.c_str(), param.name.c_str());
				return false;
			}
			if (!value.arr.empty())
				Diagnostic("malformed", NqUtil::Format("%s must not contain array entries", schema).c_str(),
					node, slot, kind.id.c_str(), param.name.c_str());
			for (u32 i = 0; i < value.keys.size(); ++i)
			{
				bool known = false;
				for (u32 k = 0; k < allowed_count; ++k)
					if (value.keys[i] == allowed[k]) { known = true; break; }
				if (!known)
					Diagnostic("unknown_field", NqUtil::Format("field '%s' is not valid for %s",
						value.keys[i].c_str(), schema).c_str(), node,
						NqUtil::Format("%s/%s", slot, value.keys[i].c_str()).c_str(), kind.id.c_str(),
						param.name.c_str());
			}
			return true;
		}

		bool WalkStringLeaf(const NqCatalog::SKind& kind, const NqCatalog::SParam& param,
			SNqValue& owner, LPCSTR key, LPCSTR type, bool required, LPCSTR node, LPCSTR slot)
		{
			SNqValue* value = owner.Get(key);
			const xr_string leaf_slot = NqUtil::Format("%s/%s", slot, key);
			if (!value)
			{
				if (required)
					Diagnostic("malformed", NqUtil::Format("required string field '%s' is missing", key).c_str(),
						node, leaf_slot.c_str(), kind.id.c_str(), param.name.c_str());
				return false;
			}
			if (!value->IsString())
			{
				Diagnostic("malformed", NqUtil::Format("field '%s' must be a string", key).c_str(), node,
					leaf_slot.c_str(), kind.id.c_str(), param.name.c_str());
				return false;
			}
			Reference(kind, param, type, *value, node, leaf_slot.c_str());
			return true;
		}

		void WalkPosition(const NqCatalog::SKind& kind, const NqCatalog::SParam& param,
			SNqValue& owner, LPCSTR node, LPCSTR slot)
		{
			SNqValue* position = owner.Get("pos");
			const xr_string leaf_slot = NqUtil::Format("%s/pos", slot);
			if (!position || !position->IsTable() || position->arr.size() != 3 || !position->keys.empty())
			{
				Diagnostic("malformed", "pos must be an array of exactly three numbers", node,
					leaf_slot.c_str(), kind.id.c_str(), param.name.c_str());
				return;
			}
			for (u32 i = 0; i < position->arr.size(); ++i)
				if (!position->arr[i].IsNumber())
				{
					Diagnostic("malformed", "pos must be an array of exactly three numbers", node,
						NqUtil::Format("%s/%u", leaf_slot.c_str(), i).c_str(), kind.id.c_str(), param.name.c_str());
					return;
				}
		}

		void WalkRadius(const NqCatalog::SKind& kind, const NqCatalog::SParam& param,
			SNqValue& owner, LPCSTR node, LPCSTR slot)
		{
			SNqValue* radius = owner.Get("radius");
			if (radius && (!radius->IsNumber() || radius->n <= 0.0))
				Diagnostic("malformed", "radius must be a positive number", node,
					NqUtil::Format("%s/radius", slot).c_str(), kind.id.c_str(), param.name.c_str());
		}

		void WalkSpawnSpec(const NqCatalog::SKind& kind, const NqCatalog::SParam& param,
			SNqValue& value, LPCSTR node, LPCSTR slot)
		{
			static const LPCSTR allowed[] = { "section", "smart", "ref", "hold" };
			if (!WalkObject(kind, param, value, node, slot, "spawn_spec", allowed,
				sizeof(allowed) / sizeof(allowed[0]))) return;
			WalkStringLeaf(kind, param, value, "section", "squad_section", true, node, slot);
			WalkStringLeaf(kind, param, value, "smart", "smart", true, node, slot);
			if (value.Has("ref")) WalkStringLeaf(kind, param, value, "ref", "ref_name", false, node, slot);
			if (SNqValue* hold = value.Get("hold"))
				if (!hold->IsBool())
					Diagnostic("malformed", "spawn_spec field 'hold' must be a bool", node,
						NqUtil::Format("%s/hold", slot).c_str(), kind.id.c_str(), param.name.c_str());
		}

		void WalkNpcRef(const NqCatalog::SKind& kind, const NqCatalog::SParam& param,
			SNqValue& value, LPCSTR node, LPCSTR slot, bool target, bool kill)
		{
			static const LPCSTR npc_allowed[] = { "story", "ref", "profile", "community", "level" };
			static const LPCSTR target_allowed[] =
				{ "story", "ref", "profile", "community", "level", "smart", "restrictor", "pos", "radius" };
			static const LPCSTR kill_allowed[] = { "story", "ref", "profile", "community", "level", "spawn" };
			const LPCSTR* allowed = target ? target_allowed : kill ? kill_allowed : npc_allowed;
			const u32 allowed_count = target ? sizeof(target_allowed) / sizeof(target_allowed[0])
				: kill ? sizeof(kill_allowed) / sizeof(kill_allowed[0]) : sizeof(npc_allowed) / sizeof(npc_allowed[0]);
			if (!WalkObject(kind, param, value, node, slot, param.type.c_str(), allowed, allowed_count)) return;

			int alternatives = 0;
			static const LPCSTR common[] = { "story", "ref", "profile", "community" };
			for (u32 i = 0; i < sizeof(common) / sizeof(common[0]); ++i)
				if (value.Has(common[i])) ++alternatives;
			if (target)
			{
				if (value.Has("smart")) ++alternatives;
				if (value.Has("restrictor")) ++alternatives;
				if (value.Has("pos")) ++alternatives;
			}
			if (kill && value.Has("spawn")) ++alternatives;
			if (alternatives != 1)
				Diagnostic("malformed", NqUtil::Format("%s must select exactly one target variant",
					param.type.c_str()).c_str(), node, slot, kind.id.c_str(), param.name.c_str());

			if (value.Has("story")) WalkStringLeaf(kind, param, value, "story", "story_id", false, node, slot);
			if (value.Has("ref")) WalkStringLeaf(kind, param, value, "ref", "ref_name", false, node, slot);
			if (value.Has("profile")) WalkStringLeaf(kind, param, value, "profile", "profile", false, node, slot);
			if (value.Has("community")) WalkStringLeaf(kind, param, value, "community", "community", false, node, slot);
			if (value.Has("level"))
			{
				if (!value.Has("community") && !(target && value.Has("pos")))
					Diagnostic("malformed", "level is valid only for community or pos variants", node,
						NqUtil::Format("%s/level", slot).c_str(), kind.id.c_str(), param.name.c_str());
				WalkStringLeaf(kind, param, value, "level", "level", false, node, slot);
			}
			if (target && value.Has("smart")) WalkStringLeaf(kind, param, value, "smart", "smart", false, node, slot);
			if (target && value.Has("restrictor"))
				WalkStringLeaf(kind, param, value, "restrictor", "string", false, node, slot);
			if (target && value.Has("pos")) WalkPosition(kind, param, value, node, slot);
			if (target && value.Has("radius"))
			{
				if (!value.Has("pos"))
					Diagnostic("malformed", "radius is valid only for the pos variant", node,
						NqUtil::Format("%s/radius", slot).c_str(), kind.id.c_str(), param.name.c_str());
				WalkRadius(kind, param, value, node, slot);
			}
			if (kill && value.Has("spawn"))
			{
				SNqValue* spawn = value.Get("spawn");
				WalkSpawnSpec(kind, param, *spawn, node, NqUtil::Format("%s/spawn", slot).c_str());
			}
		}

		void WalkPlace(const NqCatalog::SKind& kind, const NqCatalog::SParam& param,
			SNqValue& value, LPCSTR node, LPCSTR slot)
		{
			static const LPCSTR allowed[] = { "level", "pos", "radius", "restrictor", "smart" };
			if (!WalkObject(kind, param, value, node, slot, "place", allowed,
				sizeof(allowed) / sizeof(allowed[0]))) return;
			const bool position = value.Has("pos") || value.Has("level") || value.Has("radius");
			const int alternatives = (position ? 1 : 0) + (value.Has("restrictor") ? 1 : 0) + (value.Has("smart") ? 1 : 0);
			if (alternatives != 1)
				Diagnostic("malformed", "place must select exactly one of level+pos, restrictor or smart", node,
					slot, kind.id.c_str(), param.name.c_str());
			if (position)
			{
				WalkStringLeaf(kind, param, value, "level", "level", true, node, slot);
				WalkPosition(kind, param, value, node, slot);
				WalkRadius(kind, param, value, node, slot);
			}
			if (value.Has("restrictor"))
				WalkStringLeaf(kind, param, value, "restrictor", "string", false, node, slot);
			if (value.Has("smart")) WalkStringLeaf(kind, param, value, "smart", "smart", false, node, slot);
		}

		void WalkDuration(const NqCatalog::SKind& kind, const NqCatalog::SParam& param,
			SNqValue& value, LPCSTR node, LPCSTR slot)
		{
			if (value.IsNumber())
			{
				if (value.n < 0.0)
					Diagnostic("malformed", "duration must be non-negative", node, slot,
						kind.id.c_str(), param.name.c_str());
				return;
			}
			static const LPCSTR units[] = { "seconds", "game_hours", "game_minutes", "game_seconds" };
			if (!WalkObject(kind, param, value, node, slot, "duration", units, sizeof(units) / sizeof(units[0]))) return;
			int alternatives = 0;
			for (u32 i = 0; i < sizeof(units) / sizeof(units[0]); ++i)
			{
				SNqValue* unit = value.Get(units[i]);
				if (!unit) continue;
				++alternatives;
				if (!unit->IsNumber() || unit->n < 0.0)
					Diagnostic("malformed", NqUtil::Format("duration field '%s' must be a non-negative number",
						units[i]).c_str(), node, NqUtil::Format("%s/%s", slot, units[i]).c_str(),
						kind.id.c_str(), param.name.c_str());
			}
			if (alternatives != 1)
				Diagnostic("malformed", "duration must select exactly one unit", node, slot,
					kind.id.c_str(), param.name.c_str());
		}

		void WalkParam(const NqCatalog::SKind& kind, const NqCatalog::SParam& param,
			SNqValue& value, LPCSTR node, LPCSTR slot)
		{
			const xr_string& type = param.type;
			if (!IsKnownType(type))
			{
				Diagnostic("unknown_type", NqUtil::Format("parameter type '%s' is not inspectable", type.c_str()).c_str(),
					node, slot, kind.id.c_str(), param.name.c_str());
				return;
			}
			if (type == "lua")
			{
				Diagnostic("lua_uninspectable", "Lua may contain runtime references that cannot be proven statically",
					node, slot, kind.id.c_str(), param.name.c_str());
				return;
			}
			if (IsStringType(type))
			{
				if (!value.IsString())
				{
					Diagnostic("malformed", NqUtil::Format("parameter '%s' (%s) must be a string",
						param.name.c_str(), type.c_str()).c_str(), node, slot, kind.id.c_str(), param.name.c_str());
					return;
				}
				if (type == "enum" || type == "relation")
				{
					bool allowed = param.enums.empty();
					if (type == "relation" && param.enums.empty())
						allowed = value.s == "enemy" || value.s == "neutral" || value.s == "friend";
					else
						for (u32 i = 0; i < param.enums.size(); ++i)
							if (param.enums[i] == value.s) { allowed = true; break; }
					if (!allowed)
						Diagnostic("malformed", NqUtil::Format("'%s' is not an allowed %s value",
							value.s.c_str(), type.c_str()).c_str(), node, slot, kind.id.c_str(), param.name.c_str());
				}
				Reference(kind, param, type.c_str(), value, node, slot);
				return;
			}
			if (type == "cond_list") { WalkCondList(value, node, slot); return; }
			if (type == "cases_cond") { WalkCases(value, node, slot, false); return; }
			if (type == "cases_weight") { WalkCases(value, node, slot, true); return; }
			if (type == "npc_ref") { WalkNpcRef(kind, param, value, node, slot, false, false); return; }
			if (type == "target_ref") { WalkNpcRef(kind, param, value, node, slot, true, false); return; }
			if (type == "kill_target") { WalkNpcRef(kind, param, value, node, slot, false, true); return; }
			if (type == "place") { WalkPlace(kind, param, value, node, slot); return; }
			if (type == "spawn_spec") { WalkSpawnSpec(kind, param, value, node, slot); return; }
			if (type == "duration") { WalkDuration(kind, param, value, node, slot); return; }
			if (type == "int" || type == "float")
			{
				if (!value.IsNumber())
					Diagnostic("malformed", NqUtil::Format("parameter '%s' must be a number",
						param.name.c_str()).c_str(), node, slot, kind.id.c_str(), param.name.c_str());
				else
				{
					if (type == "int" && !IsInteger(value.n))
						Diagnostic("malformed", NqUtil::Format("parameter '%s' must be an integer",
							param.name.c_str()).c_str(), node, slot, kind.id.c_str(), param.name.c_str());
					if (param.has_min && value.n < param.min)
						Diagnostic("malformed", NqUtil::Format("parameter '%s' is below its catalog minimum",
							param.name.c_str()).c_str(), node, slot, kind.id.c_str(), param.name.c_str());
					if (param.has_max && value.n > param.max)
						Diagnostic("malformed", NqUtil::Format("parameter '%s' is above its catalog maximum",
							param.name.c_str()).c_str(), node, slot, kind.id.c_str(), param.name.c_str());
				}
				return;
			}
			if (type == "bool")
			{
				if (!value.IsBool())
					Diagnostic("malformed", NqUtil::Format("parameter '%s' must be a bool",
						param.name.c_str()).c_str(), node, slot, kind.id.c_str(), param.name.c_str());
				return;
			}
			if (type == "text")
			{
				if (value.IsString()) return;
				if (!value.IsTable() || value.keys.empty() || !value.arr.empty())
				{
					Diagnostic("malformed", "text must be a string or a non-empty language table", node,
						slot, kind.id.c_str(), param.name.c_str());
					return;
				}
				for (u32 i = 0; i < value.vals.size(); ++i)
					if (!value.vals[i].IsString())
						Diagnostic("malformed", "localized text value must be a string", node,
							NqUtil::Format("%s/%s", slot, value.keys[i].c_str()).c_str(), kind.id.c_str(), param.name.c_str());
				return;
			}
			if (type == "value")
			{
				if (value.IsTable() || value.IsNil())
					Diagnostic("malformed", "value must be a bool, number or string", node, slot,
						kind.id.c_str(), param.name.c_str());
				return;
			}
			if (type == "count_or_all")
			{
				if (!(value.IsString() && value.s == "all")
					&& (!value.IsNumber() || !IsInteger(value.n) || value.n < 1.0))
					Diagnostic("malformed", "count_or_all must be a positive integer or 'all'", node, slot,
						kind.id.c_str(), param.name.c_str());
				return;
			}
			Diagnostic("unknown_type", NqUtil::Format("parameter type '%s' has no structural walker",
				type.c_str()).c_str(), node, slot, kind.id.c_str(), param.name.c_str());
		}

		void WalkCondList(SNqValue& list, LPCSTR node, LPCSTR slot)
		{
			if (!list.IsTable() || list.arr.empty() || !list.keys.empty())
			{
				Diagnostic("malformed", "condition list must be a non-empty array", node, slot);
				return;
			}
			for (u32 i = 0; i < list.arr.size(); ++i)
				WalkCondValue(list.arr[i], node, NqUtil::Format("%s/cond:%u", slot, i).c_str());
		}

		void WalkCondContainer(SNqValue& list, LPCSTR node, LPCSTR slot)
		{
			if (!list.IsTable())
			{
				Diagnostic("malformed", "condition container must be a table", node, slot);
				return;
			}
			if (list.arr.empty() && list.Has("kind"))
			{
				WalkCondValue(list, node, NqUtil::Format("%s/cond:0", slot).c_str());
				return;
			}
			if (!list.keys.empty())
			{
				Diagnostic("malformed", "condition list must contain only condition entries", node, slot);
				return;
			}
			for (u32 i = 0; i < list.arr.size(); ++i)
				WalkCondValue(list.arr[i], node, NqUtil::Format("%s/cond:%u", slot, i).c_str());
		}

		void WalkCondValue(SNqValue& value, LPCSTR node, LPCSTR slot)
		{
			if (!value.IsTable())
			{
				Diagnostic("malformed", "condition must be a table", node, slot);
				return;
			}
			if (!value.arr.empty()) Diagnostic("malformed", "condition must not contain array entries", node, slot);
			for (u32 i = 0; i < value.keys.size(); ++i)
				if (value.keys[i] != "kind" && value.keys[i] != "params" && value.keys[i] != "not")
					Diagnostic("unknown_field", NqUtil::Format("unknown condition field '%s'",
						value.keys[i].c_str()).c_str(), node,
						NqUtil::Format("%s/%s", slot, value.keys[i].c_str()).c_str());
			const SNqValue* kind_value = value.Get("kind");
			if (!kind_value || !kind_value->IsString() || kind_value->s.empty())
			{
				Diagnostic("malformed", "condition needs a string kind", node, slot);
				return;
			}
			if (const SNqValue* negate = value.Get("not"))
				if (!negate->IsBool())
					Diagnostic("malformed", "condition field 'not' must be a bool", node,
						NqUtil::Format("%s/not", slot).c_str());
			SNqValue empty;
			SNqValue* params = value.Get("params");
			if (!params) params = &empty;
			WalkKind(kind_value->s.c_str(), *params, node, slot, NqCatalog::useCond);
		}

		void WalkCases(SNqValue& cases, LPCSTR node, LPCSTR slot, bool weighted)
		{
			if (!cases.IsTable() || cases.arr.empty() || !cases.keys.empty())
			{
				Diagnostic("malformed", "cases must be a non-empty array", node, slot);
				return;
			}
			for (u32 i = 0; i < cases.arr.size(); ++i)
			{
				SNqValue& item = cases.arr[i];
				const xr_string case_slot = NqUtil::Format("%s/case:%u", slot, i);
				if (!item.IsTable())
				{
					Diagnostic("malformed", "case must be a table", node, case_slot.c_str());
					continue;
				}
				if (!item.arr.empty()) Diagnostic("malformed", "case must not contain array entries", node, case_slot.c_str());
				for (u32 k = 0; k < item.keys.size(); ++k)
				{
					const bool allowed = item.keys[k] == "name" || item.keys[k] == (weighted ? "weight" : "cond");
					if (!allowed) Diagnostic("unknown_field", NqUtil::Format("unknown case field '%s'",
						item.keys[k].c_str()).c_str(), node,
						NqUtil::Format("%s/%s", case_slot.c_str(), item.keys[k].c_str()).c_str());
				}
				const SNqValue* name = item.Get("name");
				if (!name || !name->IsString())
					Diagnostic("malformed", "case needs a string name", node,
						NqUtil::Format("%s/name", case_slot.c_str()).c_str());
				else
				{
					if (!NqText::ValidId(name->s.c_str()))
						Diagnostic("malformed", "case name must be [a-z0-9_]+", node,
							NqUtil::Format("%s/name", case_slot.c_str()).c_str());
					for (u32 earlier = 0; earlier < i; ++earlier)
					{
						const SNqValue* previous = cases.arr[earlier].Get("name");
						if (previous && previous->IsString() && previous->s == name->s)
						{
							Diagnostic("malformed", NqUtil::Format("duplicate case name '%s'", name->s.c_str()).c_str(),
								node, NqUtil::Format("%s/name", case_slot.c_str()).c_str());
							break;
						}
					}
				}
				if (weighted)
				{
					const SNqValue* weight = item.Get("weight");
					if (!weight || !weight->IsNumber() || weight->n <= 0.0)
						Diagnostic("malformed", "weighted case needs weight > 0", node,
							NqUtil::Format("%s/weight", case_slot.c_str()).c_str());
				}
				else
				{
					SNqValue* conditions = item.Get("cond");
					if (!conditions) Diagnostic("malformed", "conditional case needs cond", node,
						NqUtil::Format("%s/cond", case_slot.c_str()).c_str());
					else WalkCondContainer(*conditions, node, case_slot.c_str());
				}
			}
		}
	};
}

xr_string NqReferences::SDiagnostic::Text() const
{
	xr_string where = NqUtil::ProjectRelative(path.c_str());
	if (!node.empty()) { where += ":"; where += node; }
	if (!slot.empty()) { where += "["; where += slot; where += "]"; }
	return NqUtil::Format("%s %s: %s", where.c_str(), code.c_str(), message.c_str());
}

bool NqReferences::Find(const NqProjectIndex::SSnapshot& snapshot, LPCSTR type, LPCSTR value,
	LPCSTR scope_path, SResult& out, xr_string& err)
{
	out = SResult();
	err.clear();
	out.generation = snapshot.generation;
	out.fingerprint = snapshot.fingerprint;
	out.catalog_generation = NqCatalog::Generation();
	if (!type || !type[0]) { err = "reference type is empty"; return false; }
	if (!value || !value[0]) { err = "reference value is empty"; return false; }
	if (!IsKnownType(type)) { err = xr_string("unknown catalog parameter type '") + type + "'"; return false; }
	if (!NqCatalog::Ensure()) { err = xr_string("catalog unavailable: ") + NqCatalog::LoadError(); return false; }

	const xr_string scope = scope_path && scope_path[0] ? NqUtil::PathKey(scope_path) : xr_string();
	bool found_scope = scope.empty();
	for (u32 i = 0; i < snapshot.entries.size(); ++i)
	{
		const NqProjectIndex::SEntry& entry = snapshot.entries[i];
		if (!scope.empty() && NqUtil::PathKey(entry.path.c_str()) != scope) continue;
		found_scope = true;
		if (!entry.readable || !entry.complete)
		{
			SDiagnostic diagnostic;
			diagnostic.path = entry.path;
			diagnostic.code = entry.readable ? "incomplete_model" : "unreadable";
			diagnostic.message = entry.error.empty() ? "quest could not be inspected" : entry.error;
			diagnostic.source = entry.Source();
			out.diagnostics.push_back(diagnostic);
			continue;
		}
		SNqQuest quest = entry.quest;
		SWalker walker(entry.path.c_str(), entry.Source(), type, value, out.references, out.diagnostics);
		walker.Quest(quest);
	}
	if (!found_scope) { err = "quest is not present in the project index"; return false; }
	out.complete = out.diagnostics.empty();
	return true;
}

bool NqReferences::RenameTask(SNqQuest& quest, LPCSTR path, LPCSTR from, LPCSTR to,
	int& references_updated, xr_vector<SDiagnostic>& diagnostics)
{
	references_updated = 0;
	diagnostics.clear();
	xr_vector<SReference> references;
	xr_vector<SNqValue*> edits;
	if (!NqCatalog::Ensure())
	{
		SDiagnostic diagnostic;
		diagnostic.path = path ? path : "";
		diagnostic.code = "catalog_unavailable";
		diagnostic.message = xr_string("catalog unavailable: ") + NqCatalog::LoadError();
		diagnostic.source = "memory";
		diagnostics.push_back(diagnostic);
		return false;
	}
	SWalker walker(path, "memory", "task_id", from, references, diagnostics, &edits);
	walker.Quest(quest);
	if (!diagnostics.empty()) return false;

	SNqTask* task = quest.FindTask(from);
	if (!task)
	{
		SDiagnostic diagnostic;
		diagnostic.path = path ? path : "";
		diagnostic.code = "missing_declaration";
		diagnostic.message = xr_string("task '") + from + "' is not declared";
		diagnostic.source = "memory";
		diagnostics.push_back(diagnostic);
		return false;
	}
	for (u32 i = 0; i < edits.size(); ++i) edits[i]->s = to;
	task->id = to;
	quest.InvalidateLookupIndices();
	references_updated = static_cast<int>(edits.size());
	return true;
}
