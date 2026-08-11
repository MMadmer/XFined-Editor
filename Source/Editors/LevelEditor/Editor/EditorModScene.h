#pragma once

// XMS scene-side mod exports (level overlays).
//
// Bakes editor-placed scene objects into module deltas inside the active
// project, for the game engine to merge into a base level at load time - the
// base level files are never rewritten, so two modules can edit one map:
//   spawn ops  -> spawn\<level>.xspawn                     (preferred for props)
//   collision  -> levels\<level>\overlay.xcform            (XCF1 binary)
//   visuals    -> levels\<level>\visuals\<object>.ogf      (+ overlay_visuals.ltx)
// Menu entries run on the current selection and pop a result modal; the MCP
// handlers take level/sector/selected_only arguments. Mirrors EditorMod style.

namespace EditorModScene
{
	// File\Mod menu triggers (selected objects, sector 0) + the result modal
	void RequestExportSpawn		();
	void RequestExportCut		();
	void RequestExportXCForm	();
	void RequestExportOGF		();
	void DrawUI					();

	// MCP command handlers: fill the full JSON response; raw = request line
	void McpExportSpawn			(LPCSTR raw, xr_string& out);	// mod_export_spawn
	void McpExportCut			(LPCSTR raw, xr_string& out);	// mod_export_cut
	void McpExportXCForm		(LPCSTR raw, xr_string& out);	// mod_export_xcform
	void McpExportOGF			(LPCSTR raw, xr_string& out);	// mod_export_ogf_probe
}
