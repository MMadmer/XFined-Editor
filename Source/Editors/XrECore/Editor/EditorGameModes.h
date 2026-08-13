#pragma once

// The game modes a module can target, read out of the LINKED game install.
//
// Dead Air has no config listing its modes: a mode is a checkbox declared in
// scripts\ui_mm_faction_select.script, laid out in configs\ui\
// ui_mm_faction_select_16.xml and stored as a [character_creation] key in
// axr_options.ltx. The XML layout is what separates the two kinds - the
// <dar2_mode> column holds campaigns (Dead Air Metro, War Monolith, ...) and
// the <dar2_option> column holds rule toggles (one life, richer stashes,
// higher rewards). Only the first kind is a mode a module can be built for.
//
// This matters per install: a global mod like Revolution II replaces that
// script and XML wholesale through its archive, so the real list depends on
// what the author has actually linked - which is why it is scanned, never
// hardcoded.

class ECORE_API EditorGameModes
{
public:
	struct SMode
	{
		xr_string	id;			// module-facing id: new_game_metro_mode -> "metro"
		xr_string	config_key;	// the [character_creation] key it comes from
		xr_string	caption;	// what the player sees, UTF-8; falls back to the id
		xr_string	source;		// "game" or the id of the XMS module declaring it
		bool		campaign;	// left column: a mode, not an option
		bool		available;	// the script really creates its checkbox
					SMode() : campaign(false), available(true) {}
	};

	// Scans the linked install (and the XMS modules under its mods\ folder).
	// Cached; Invalidate() forces the next call to re-read. False + reason when
	// nothing could be read - the caller still gets the "no mode" entry.
	static bool		Scan				(xr_string& err);
	static void		Invalidate			();

	// Campaigns first, then modes declared by installed XMS modules. Options
	// are parsed but never returned: they are not something a mod targets.
	static const xr_vector<SMode>&	All	();

	// MCP: the same list as JSON, so a script can pick a target without the UI
	static void		McpList				(xr_string& out);
};
