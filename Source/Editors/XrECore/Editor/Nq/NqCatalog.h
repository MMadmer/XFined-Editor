#pragma once

// NQ - the kind catalog (docs/NQ_ARCHITECTURE.md par. 6, par. 13.2).
//
// The game owns the catalog: configs\nq\catalog.ltx (+ configs\nq\kinds\*.ltx)
// of the linked install, plus the kinds\*.ltx of every installed module. The
// editor's bundled copy under <sdk>\gamedata\configs\nq\ is only the fallback
// when no linked game provides one. Cached until Invalidate().

struct SNqValue;

class ECORE_API NqCatalog
{
public:
	enum EUse
	{
		useTrigger	= 1 << 0,
		useMain		= 1 << 1,
		useExtra	= 1 << 2,
		useCond		= 1 << 3,
	};

	struct SParam
	{
		xr_string			name;
		xr_string			type;		// catalog type name (item_section, text, enum, ...)
		bool				required;
		bool				has_default;
		xr_string			def;		// default as written in the catalog
		bool				has_min, has_max;
		double				min, max;
		xr_vector<xr_string> enums;		// enum=a|b|c
		SParam() : required(false), has_default(false), has_min(false), has_max(false), min(0), max(0) {}
	};

	// nested types are not covered by the class export
	struct ECORE_API SKind
	{
		xr_string			id;			// "dialog.topic"
		xr_string			group;
		xr_string			title;		// UTF-8
		xr_string			desc;		// UTF-8
		u32					use;		// EUse mask
		xr_vector<SParam>	params;		// catalog order
		xr_vector<xr_string> pins;		// declared pins; "cases" = pins come from params.cases
		bool				pins_from_cases;
		bool				wait;
		bool				once_default;
		bool				event;		// cond: edge-triggered event condition
		xr_string			impl;
		int					since;
		xr_vector<xr_string> alias;		// old kind names this one replaces (CSV in the file)
		xr_string			source;		// file the section came from (for the UI badge)

		SKind() : use(0), pins_from_cases(false), wait(false), once_default(false), event(false), since(1) {}

		bool				Is			(EUse u) const { return 0 != (use & u); }
		const SParam*		Param		(LPCSTR name) const;
		// declared (static) pin?
		bool				HasPin		(LPCSTR pin) const;
	};

	// forces a reload on next use (link change, rescan)
	static void						Invalidate	();
	// loads when needed; false only when not even the bundled file is readable
	static bool						Ensure		();
	static LPCSTR					Source		();		// "game" | "bundled" | "" (not loaded)
	static int						Version		();
	static int						Api			();
	// changes after every catalog reload, including a project-link switch
	static u32						Generation	();
	static LPCSTR					LoadError	();
	static const xr_vector<SKind>&	Kinds		();
	static const SKind*				Find		(LPCSTR id);
	// kinds usable at a position, in catalog order (UI dropdowns)
	static void						KindsFor	(u32 use_mask, xr_vector<const SKind*>& out);
	// files that made up the catalog (diagnostics)
	static const xr_vector<xr_string>& Files	();

	// Groups of parameters a kind takes only one of at a time. The catalog line has
	// no syntax for that yet, so the shape lives here - read by the validator (E022)
	// and by the inspector, which greys out the losing groups. Each entry is a comma
	// separated list of parameter names; empty when the kind has no such rule.
	static void						ExclusiveForms(LPCSTR kind, xr_vector<xr_string>& out);
	// index of the form `param` belongs to, -1 when the kind has no rule about it
	static int						FormIndex	(LPCSTR kind, LPCSTR param);
	// true when `param` belongs to a form that a value already present has ruled out
	static bool						FormRuledOut(LPCSTR kind, LPCSTR param, const SNqValue& params);

	// MCP quest_catalog: full JSON response
	static void						McpCatalog	(xr_string& out);
};
