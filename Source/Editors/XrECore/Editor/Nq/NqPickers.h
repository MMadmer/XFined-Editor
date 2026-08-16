#pragma once

// NQ - game data indexes behind the parameter pickers (docs/NQ_ARCHITECTURE.md
// par. 13.6): items, squads, levels, smarts, story ids, profiles, communities,
// info portions, spot types. Built lazily from the linked game (pSettings is
// the game's system.ltx once EditorGameConfigs activated it; XML and ltx that
// system.ltx does not include come through EditorGameContent), cached on disk
// in <sdk>\_appdata_\nq_index_<hash>.txt keyed by the game-config fingerprint.
// Story ids also come from the project's own spawn\custom_data\*.ltx.
//
// Captions come from the game's own string table (configs\text\<lang>\*.xml,
// cp1251), reached the same way the game reaches them: items and quest objects
// through inv_name, NPCs and their story ids through character_profile ->
// <name>, levels and communities through their own id, smart terrains through
// misc\smart_names.ltx. Ids the game gives no player-facing text (squads, info
// portions, map spot types, most props) keep an empty name on purpose.

class ECORE_API NqPickers
{
public:
	enum EType
	{
		tItem = 0,		// item_section
		tSquad,			// squad_section
		tLevel,			// level
		tSmart,			// smart
		tStory,			// story_id
		tProfile,		// profile
		tCommunity,		// community
		tInfo,			// info
		tSpot,			// spot_type
		tCount
	};

	struct SEntry
	{
		xr_string	id;
		xr_string	name;		// UTF-8 caption (may be empty)
		xr_string	extra;		// origin hint: section, file, level, "project"
	};

	// true when a linked game backs the indexes (otherwise every index is empty
	// and W060 is not raised)
	static bool						Available	();
	static void						Invalidate	();
	static const xr_vector<SEntry>&	Index		(EType t);
	static bool						Known		(EType t, LPCSTR id);
	// catalog parameter type -> index (tCount when the type has no index)
	static EType					TypeFromName(LPCSTR type_name);
	static LPCSTR					TypeName	(EType t);
	// substring search (id or name), case-insensitive; `limit` <= 0 = 100
	static void						Search		(EType t, LPCSTR query, int limit, xr_vector<const SEntry*>& out);
	// MCP quest_lookup: {"type","query","limit"[,"path"]} - document-scoped
	// types (task_id, var_name, ref_name, node_id, quest_id) read the quest at
	// `path` (open document or file) and the project's other quests
	static void						McpLookup	(LPCSTR raw, xr_string& out);
};
