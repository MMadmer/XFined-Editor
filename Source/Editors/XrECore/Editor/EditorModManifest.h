#pragma once

// XMS (XFined Module System) mod-authoring support.
//
// Every project doubles as an XMS module package: a mod.ltx manifest at the
// project root plus the module dirs (gamedata\, patch\, spawn\, scripts\).
// This unit owns the manifest parse/write, the scaffold and both export
// paths; the LevelEditor menu and the MCP inspector are thin callers.
// Plain statics in a namespace, same style as EditorProject.

namespace EditorMod
{
	// a game mode this module declares ([provides_mode]: id + title string id)
	struct SProvidesMode
	{
		xr_string				id;
		xr_string				title;
	};

	// parsed mod.ltx; unknown keys/sections ([budget], [declares], engine_min,
	// ...) survive a load/save round-trip verbatim in the *_extra fields
	struct SManifest
	{
		xr_string				id;
		xr_string				name;
		xr_string				version;
		xr_string				api;
		xr_string				mode;			// [module] mode: gate content by game mode, empty = all modes
		// [module] target: which of the three targeting choices the author made.
		// "default" = the ordinary game, deliberately. Empty means nothing was
		// chosen yet, and the module export refuses that - shipping a module
		// whose mode nobody decided on is how content lands in the wrong game.
		xr_string				target;
		xr_vector<xr_string>	requires_list;	// raw entries, e.g. "other.mod = >=1.0"
		xr_vector<xr_string>	after;			// module ids from [order]
		xr_vector<xr_string>	before;
		xr_vector<xr_string>	conflicts;		// raw entries
		xr_vector<SProvidesMode> provides_modes;	// declared game modes
		xr_vector<xr_string>	provides_extra;	// unknown [provides_mode] lines
		xr_vector<xr_string>	module_extra;	// unknown [module] lines
		xr_vector<xr_string>	extra;			// unknown sections, headers included
	};

	// id charset per XMS spec: [a-z0-9_.\-]+
	ECORE_API bool	ValidateId			(LPCSTR id);
	// mode ids follow the module id charset, but empty is allowed (= all modes)
	ECORE_API bool	ValidateModeId		(LPCSTR id);
	// project name -> module id: lowercased, invalid chars become '_'
	ECORE_API void	DeriveId			(LPCSTR project_name, char* dst, u32 dst_size);

	// creates missing mod.ltx / module dirs / patch readme; never overwrites
	ECORE_API void	EnsureScaffold		(LPCSTR project_root, LPCSTR project_name);

	ECORE_API bool	Load				(LPCSTR project_root, SManifest& m);
	ECORE_API bool	Save				(LPCSTR project_root, const SManifest& m);

	// flat=false: mod.ltx + module dirs -> <target>\mods\<id>\
	// flat=true : gamedata content -> <target>\gamedata_<id>\ + compat report
	// The module export MIRRORS the project: files the target holds and the
	// project does not are deleted. When those exist and `confirmed` is false
	// it refuses instead, so the caller can ask first - the target may be
	// someone else's module that shares the id.
	ECORE_API bool	Export				(LPCSTR project_root, LPCSTR target_root, bool flat,
										 int& files, xr_string& out_path, xr_string& err,
										 bool confirmed = false);

	// modal windows (drawn from CLevelMain::OnDrawUI) + File\Mod menu triggers
	ECORE_API void	DrawUI				();
	ECORE_API void	RequestEditManifest	();
	ECORE_API void	RequestExportModule	();
	ECORE_API void	RequestExportFlat	();

	// MCP command handlers: fill the full JSON response; raw = request line
	ECORE_API void	McpManifest			(xr_string& out);
	ECORE_API void	McpSetManifest		(LPCSTR raw, xr_string& out);
	ECORE_API void	McpExport			(LPCSTR raw, xr_string& out);
}
