#pragma once

// Unreal-style project system.
//
// On startup the editor shows a fullscreen Project Browser (recent-project
// tiles with previews + a create form) and nothing else; the actual editor UI
// appears only after a project is opened. Opening a project REMOUNTS the
// writable FS aliases ($maps$, $game_levels$, $game_spawn$, $import$, ...)
// onto the project folder at runtime via FS_Path::_set_root, so scene saves
// and compiled output can never touch the shared SDK data.
//
// Project = folder with project.ltx. Recents live in _projects.ini next to
// the SDK root. preview.png (captured on project close) feeds the tiles.

class ECORE_API EditorProject
{
public:
	using TLiveSceneQuery = bool (*)();

	static bool		Active				();
	static LPCSTR	Root				();		// project folder, no trailing slash
	static LPCSTR	Name				();
	static LPCSTR	BaseMapsDir			();		// SDK rawdata\levels (scene import source)

	// The editor ships the source scenes the game's levels were built from, and
	// opening a project remounts $maps$ onto the project - which is exactly why
	// those scenes have to be reachable by name rather than through the FS.
	// Names only, no extension, sorted.
	static void		ListBaseScenes		(xr_vector<xr_string>& out);
	static bool		HasBaseScene		(LPCSTR name);
	// Copies <base>\<name>.level (+ its folder) into the project and answers
	// with the project-side path. Already imported = no copy, same answer, so
	// the author always edits their own copy and the shared library stays put.
	static bool		ImportBaseScene		(LPCSTR name, string_path& out_level, xr_string& err);

	// fullscreen browser page; drawn INSTEAD of the editor UI until a project
	// is opened. Returns true while the browser is on screen.
	static bool		DrawBrowser			();

	// ---- linked game install ------------------------------------------------
	// A project points at one X-Ray game install ([project] game_root); the
	// DARF Content browser reads that install, read-only, and nothing else in
	// the editor is allowed to write into it.
	static LPCSTR	GameRoot			();		// linked folder, no trailing slash
	// non-empty + folder exists + fsgame.ltx present + a database or gamedata dir
	static bool		GameLinked			();
	// validates, stores in project.ltx, mounts the content; on failure returns
	// false and leaves the human-readable cause in LinkError()
	static bool		LinkGame			(LPCSTR folder);
	static LPCSTR	LinkError			();
	// BLOCKING modal, drawn INSTEAD of the editor UI whenever a project is open
	// but its game link is missing or stale. Returns true while it owns the frame.
	static bool		DrawGameLink		();
	// MCP: report the link, or set it when the request carries a 'path'
	static void		McpGameLink			(LPCSTR raw, xr_string& out);
	// The host editor owns the concrete scene type; XrECore only asks whether a context switch is safe.
	static void		SetLiveSceneQuery	(TLiveSceneQuery query);

	// opens an existing project folder (must hold project.ltx unless create)
	static bool		Open				(LPCSTR folder);
	static LPCSTR	OpenError			();
	// creates <parent>\<name> (latin letters, digits, _ and - only) and opens it
	static bool		Create				(LPCSTR parent, LPCSTR name);
	// captures preview, stores last scene, unmounts and re-opens the browser
	static void		Close				();

	static bool		ValidateName		(LPCSTR name);

	// ---- what belongs to the module, and what only to the editor ------------
	// A project folder holds two kinds of things side by side: the module the
	// author is building (any layout they like - see the [vfs] section of
	// mod.ltx) and the editor's own scratch. These two answers keep the content
	// browser and the module export from disagreeing about which is which.
	//
	// Editor-only: never shown as content, never exported.
	static bool		IsEditorOnlyEntry	(LPCSTR name_in_root);
	// Sources: shown and editable (this is where scenes and .object files
	// live), but not shipped - the game has no use for them.
	static bool		IsSourceOnlyEntry	(LPCSTR name_in_root);

	// in-editor helpers
	static void		DrawUI				();		// import-scene modal
	static void		RequestImportScene	();
	static void		OpenProjectFolder	();
	// deferred last-scene autoload: returns the path once, after the browser
	// modal is fully closed (loading from inside the popup stack crashes)
	static LPCSTR	PopPendingScene		();
	// recent projects as a JSON array — for the MCP list_projects command
	static void		ListRecentJson		(xr_string& out);

private:
	static void		Mount				();
	static void		Unmount				();
	static void		CreateLayout		();
	static void		LoadManifest		();
	static void		SaveManifest		();
	static void		LoadRecent			();
	static void		SaveRecent			();
	static void		SavePreview			();
	static void		ReleasePreviews		();
	static bool		PickFolder			(char* dest, u32 dest_size, const wchar_t* title);
};
