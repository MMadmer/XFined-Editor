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

	// Where one more campaign checkbox fits on the new-game screen, measured on
	// the layout the linked game really ships: the row goes under the last one
	// of the campaign column, the frame around that column grows to hold it and
	// whatever sat below the frame is pushed down by the same amount. Every
	// number here comes from the file - nothing about Dead Air's own layout is
	// assumed, so a screen replaced by a global mod is measured on its terms.
	struct SSlotShift
	{
		xr_string	node;			// direct child of main_dialog
		int			y;				// its new y
	};
	struct SSlot
	{
		xr_string			layout;			// gamedata-relative layout file
		int					check_x, check_y, check_w, check_h;
		int					cap_x, cap_y, cap_w, cap_h;
		int					step;			// row pitch of the column
		xr_string			frame_node;		// grown to fit the row; empty = none found
		int					frame_height;
		xr_vector<SSlotShift> shift;		// pushed down by the growth
							SSlot() : check_x(0), check_y(0), check_w(30), check_h(21),
									  cap_x(0), cap_y(0), cap_w(160), cap_h(20),
									  step(25), frame_height(0) {}
	};
	// rows is how many checkboxes are being added at once; skip_module keeps a
	// module from tripping over the rows it itself added last export
	static bool		SuggestSlot			(LPCSTR layout_rel, int rows, LPCSTR skip_module,
										 SSlot& out, xr_string& err);

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
