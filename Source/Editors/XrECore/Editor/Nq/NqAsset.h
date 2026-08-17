#pragma once

// NQ - node quest graph. In-memory model of one .nqasset (docs/NQ_ARCHITECTURE.md
// par. 4, par. 13.1). UI-free and owned by XrECore, so the export gate, the MCP layer and
// the (Phase 4) canvas all share the same structures.
//
// The file is declarative Lua (`return { ... }`), so the model is a thin typed
// layer over a generic Lua value: quest / node / action / condition fields are
// typed, everything under `params` stays a generic SNqValue and is interpreted
// through the catalog (NqCatalog) when validated or drawn.

//------------------------------------------------------------------------------
// SNqValue - what the Lua reader returns: nil / bool / number / string / table.
// A table keeps its array part in order and its string keys sorted canonically
// (Lua tables have no order; canonical order makes read/write idempotent).
//------------------------------------------------------------------------------
struct ECORE_API SNqValue
{
	enum EType { tNil = 0, tBool, tNumber, tString, tTable };

	EType					type;
	bool					b;
	double					n;
	xr_string				s;
	xr_vector<SNqValue>		arr;		// table: [1..n]
	xr_vector<xr_string>	keys;		// table: string keys, canonical order
	xr_vector<SNqValue>		vals;		// table: values parallel to keys

	SNqValue() : type(tNil), b(false), n(0.0) {}

	static SNqValue	Nil		()				{ return SNqValue(); }
	static SNqValue	Bool	(bool v)		{ SNqValue r; r.type = tBool;   r.b = v; return r; }
	static SNqValue	Number	(double v)		{ SNqValue r; r.type = tNumber; r.n = v; return r; }
	static SNqValue	String	(LPCSTR v)		{ SNqValue r; r.type = tString; r.s = v ? v : ""; return r; }
	static SNqValue	String	(const xr_string& v) { SNqValue r; r.type = tString; r.s = v; return r; }
	static SNqValue	Table	()				{ SNqValue r; r.type = tTable; return r; }

	bool	IsNil	() const { return type == tNil; }
	bool	IsBool	() const { return type == tBool; }
	bool	IsNumber() const { return type == tNumber; }
	bool	IsString() const { return type == tString; }
	bool	IsTable	() const { return type == tTable; }
	// table with neither array nor keys (or nil)
	bool	IsEmpty	() const { return type == tNil || (type == tTable && arr.empty() && keys.empty()); }

	// string-key access on tables (0 when absent or not a table)
	const SNqValue*	Get		(LPCSTR key) const;
	SNqValue*		Get		(LPCSTR key);
	bool			Has		(LPCSTR key) const { return 0 != Get(key); }
	// inserts (keeping canonical key order) or returns the existing slot; turns
	// a nil value into a table
	SNqValue&		Set		(LPCSTR key);
	SNqValue&		Set		(LPCSTR key, const SNqValue& v);
	void			Erase	(LPCSTR key);
	// array helpers
	SNqValue&		Push	(const SNqValue& v);
	u32				ArrSize	() const { return (u32)arr.size(); }

	// typed reads with defaults
	xr_string		AsString(LPCSTR def = "") const;
	double			AsNumber(double def = 0.0) const;
	bool			AsBool	(bool def = false) const;
	// string field of a table, or def
	xr_string		GetString(LPCSTR key, LPCSTR def = "") const;
	double			GetNumber(LPCSTR key, double def = 0.0) const;
	bool			GetBool	(LPCSTR key, bool def = false) const;

	// deep structural equality (numbers compared exactly)
	bool			Equals	(const SNqValue& o) const;
	bool			operator==(const SNqValue& o) const { return Equals(o); }
	bool			operator!=(const SNqValue& o) const { return !Equals(o); }

	// re-sorts string keys canonically (after bulk edits)
	void			Canonicalize();
	// merges string keys of `patch` into this table (nil patch value = erase)
	void			Patch	(const SNqValue& patch);
};

typedef SNqValue SNqParams;		// always a table (or nil = no params)

//------------------------------------------------------------------------------
// canonical key order shared by reader/writer: well-known keys first (in a
// fixed order), everything else alphabetically
//------------------------------------------------------------------------------
ECORE_API int	NqKeyRank		(LPCSTR key);
ECORE_API bool	NqKeyLess		(const xr_string& a, const xr_string& b);

//------------------------------------------------------------------------------
// typed model
//------------------------------------------------------------------------------
struct ECORE_API SNqAction
{
	xr_string	kind;
	SNqParams	params;
};

struct ECORE_API SNqCond
{
	xr_string	kind;
	SNqParams	params;
	bool		negate;			// ["not"] = true
	SNqCond() : negate(false) {}
};

typedef std::pair<xr_string, xr_vector<xr_string> >	SNqPin;		// pin -> targets (file order)

struct ECORE_API SNqNode
{
	xr_string				id;
	xr_string				kind;
	bool					once;
	bool					once_set;	// `once` present in the file (else the catalog default applies)
	SNqParams				params;
	xr_vector<SNqCond>		cond;
	xr_vector<SNqAction>	on_enter;
	xr_vector<SNqAction>	on_exit;
	xr_vector<SNqPin>		out;
	Fvector2				pos;
	bool					has_pos;
	xr_string				comment;

	SNqNode() : once(false), once_set(false), has_pos(false) { pos.set(0.f, 0.f); }

	// pin helpers (out is small; linear scans are fine)
	const xr_vector<xr_string>*	Targets		(LPCSTR pin) const;
	xr_vector<xr_string>&		TargetsOf	(LPCSTR pin);		// creates the pin
	bool						Connect		(LPCSTR pin, LPCSTR to);	// false when already there
	bool						Disconnect	(LPCSTR pin, LPCSTR to);	// false when absent
	bool						HasTarget	(LPCSTR to) const;
	u32							OutDegree	() const;
	// slot list for the UI: "enter" / "exit"
	xr_vector<SNqAction>&		Slot		(LPCSTR slot);
	const xr_vector<SNqAction>*	SlotC		(LPCSTR slot) const;
};

struct ECORE_API SNqTask
{
	xr_string	id;
	SNqValue	title;			// text: string or { rus = "...", eng = "..." }
	SNqValue	descr;
	xr_string	type;			// additional | storyline
	SNqValue	target;			// target_ref table or nil
	xr_string	icon;
};

struct ECORE_API SNqVar
{
	xr_string	name;
	SNqValue	value;			// bool / number / string default
};

struct ECORE_API SNqQuest
{
	int						nq;			// format version (1)
	xr_string				id;
	SNqValue				title;		// text
	xr_string				activation;	// auto | manual
	xr_vector<SNqVar>		vars;
	xr_vector<SNqTask>		tasks;
	xr_vector<SNqNode>		nodes;

	SNqQuest();
	SNqQuest(const SNqQuest& other);
	SNqQuest(SNqQuest&& other) noexcept;
	SNqQuest& operator=(const SNqQuest& other);
	SNqQuest& operator=(SNqQuest&& other) noexcept;

	void				Clear();
	// Public vectors keep the model easy to serialize and edit. Call this after
	// changing a node/task/variable identifier in place; structural vector
	// changes are detected from their storage stamp as a second safety net.
	void				InvalidateLookupIndices();

	SNqNode*			FindNode	(LPCSTR id);
	const SNqNode*		FindNode	(LPCSTR id) const;
	int					NodeIndex	(LPCSTR id) const;			// -1 when absent
	SNqTask*			FindTask	(LPCSTR id);
	const SNqTask*		FindTask	(LPCSTR id) const;
	SNqVar*				FindVar		(LPCSTR name);
	const SNqVar*		FindVar		(LPCSTR name) const;

	// number of edges arriving at `id` (all pins of all nodes)
	int					InDegree	(LPCSTR id) const;
	// nodes reachable from the triggers (or from `from` when given), by node index
	void				Reachable	(xr_vector<bool>& out, LPCSTR from = 0) const;
	// dialog chain: phrase nodes reachable from a dialog.topic through
	// dialog.*_phrase nodes only, with the BFS depth (0 = topic); a node that
	// is reached at two different parities is reported at both
	struct SDialogHop { int node; int depth; };
	void				DialogChain	(LPCSTR topic_id, xr_vector<SDialogHop>& out) const;
	// stable topological order from the triggers (par. 13.3): trigger order = file
	// order, pins in `out` order, targets in list order; unreachable nodes keep
	// their relative order at the end
	void				TopoOrder	(xr_vector<int>& out) const;
	// deep equality of the whole quest (undo snapshots, idempotency tests)
	bool				Equals		(const SNqQuest& o) const;

private:
	struct SLookupHash
	{
		using is_transparent = void;

		size_t operator()(const xr_string& value) const noexcept
		{
			return std::hash<std::string_view>{}(std::string_view(value.data(), value.size()));
		}
		size_t operator()(LPCSTR value) const noexcept
		{
			return std::hash<std::string_view>{}(std::string_view(value));
		}
	};

	struct SLookupEqual
	{
		using is_transparent = void;

		bool operator()(const xr_string& left, const xr_string& right) const noexcept { return left == right; }
		bool operator()(const xr_string& left, LPCSTR right) const noexcept { return left == right; }
		bool operator()(LPCSTR left, const xr_string& right) const noexcept { return right == left; }
	};

	struct SLookupIndex
	{
		xr_flat_hash_map<xr_string, int, SLookupHash, SLookupEqual>	entries;
		const void*						storage;
		u32								count;
		u32								revision;

		SLookupIndex() : storage(nullptr), count(0), revision(0) {}
	};

	u32								m_LookupRevision;
	mutable SLookupIndex				m_NodeIndex;
	mutable SLookupIndex				m_TaskIndex;
	mutable SLookupIndex				m_VarIndex;

	void				ResetLookupIndices();
	void				EnsureNodeIndex() const;
	void				EnsureTaskIndex() const;
	void				EnsureVarIndex() const;
};

//------------------------------------------------------------------------------
// text helpers shared by outline, pickers and the UI
//------------------------------------------------------------------------------
namespace NqText
{
	// preferred display string of a `text` value: string as is, table -> rus,
	// then eng, then the first entry
	ECORE_API xr_string	Preview		(const SNqValue& v);
	// [a-z0-9_]+
	ECORE_API bool		ValidId		(LPCSTR id);
	// one-line summary of a param table for outlines: story:esc_m_trader,
	// ref:boars, spawn simulation_boar@smart ref=boars, level:x, ...
	ECORE_API xr_string	RefSummary	(const SNqValue& v);
	// short human summary of a value (strings quoted and clipped, tables as k=v)
	ECORE_API xr_string	ValueSummary(const SNqValue& v, u32 max_len = 48);
	// dialog helpers
	ECORE_API bool		IsPhraseKind(LPCSTR kind);		// dialog.npc_phrase / dialog.actor_phrase
	ECORE_API bool		IsTopicKind	(LPCSTR kind);		// dialog.topic
	ECORE_API bool		IsDialogKind(LPCSTR kind);		// either
	ECORE_API bool		IsTriggerKind(LPCSTR kind);		// trigger.*  (catalog-free shortcut)
	// what actually decides a trigger: the catalog (use = trigger and not main,
	// the runtime's rule), so a module kind that is not named trigger.* still
	// roots the graph. Falls back to the name when the catalog does not know it.
	ECORE_API bool		IsTrigger	(LPCSTR kind);
}
