#pragma once

// Copy / move / delete for the content browser and its MCP commands.
//
// SAFETY CONTRACT — the whole reason this unit exists:
//
//   * The project folder (EditorProject::Root()) is the ONLY place anything
//     here ever writes to, renames inside or deletes from.
//   * The linked game install (DARF) and the shared SDK library (Editor
//     Content) are READ sources. Copying OUT of them into the project is fine;
//     moving out of them is not, because a move deletes the source.
//   * Every path an argument names is normalised first ('/' -> '\\', duplicate
//     separators collapsed, "." dropped, ".." popped) and only then compared
//     against the project root. The normalisation is what makes the comparison
//     mean anything: without it "<root>\\..\\..\\Windows" passes a plain
//     starts-with test while pointing anywhere on the disk.
//
// Reparse points (junctions, symlinks) are never followed while walking a tree,
// for the same reason: the check proves where the PATH is, not where the disk
// sends it.

namespace EditorFileOps
{
	struct SFailure
	{
		xr_string			path;
		xr_string			reason;
	};

	struct SReport
	{
		int					files;		// files actually copied / moved / deleted
		int					dirs;		// directories created / removed
		int					skipped;	// left alone on purpose (exists, !recursive, ...)
		int					failed;		// items that errored out
		// first few entries only — the response is read by a human or an agent,
		// not by a log parser; the counters above stay exact
		xr_vector<SFailure>	skips;
		xr_vector<SFailure>	failures;

							SReport		() : files(0), dirs(0), skipped(0), failed(0) {}
		bool				ok			() const { return 0 == failed; }
	};

	// Absolute, normalised path PROVEN to sit inside the project root. Relative
	// input is taken against the project root, never against the process CWD.
	ECORE_API bool	ResolveInProject	(LPCSTR path, string_path& out, xr_string& err);
	// Same normalisation for a read-only source, which may sit anywhere.
	ECORE_API bool	ResolveSource		(LPCSTR path, string_path& out, xr_string& err);

	// `dst_dir` is a FOLDER inside the project; the entry keeps its own name.
	// `src` may live outside the project (read-only sources are legal here).
	ECORE_API void	Copy	(LPCSTR src, LPCSTR dst_dir, bool recursive, bool overwrite, SReport& rep);
	// Move needs the source inside the project too — see the contract above.
	ECORE_API void	Move	(LPCSTR src, LPCSTR dst_dir, bool recursive, bool overwrite, SReport& rep);
	ECORE_API void	Delete	(LPCSTR path, bool recursive, SReport& rep);

	// Where a file living under `fs_alias` belongs once it is inside the project.
	// fs.ltx builds every alias below the SDK root, so the tail of that root is
	// what gets mirrored: $objects$ -> <project>\rawdata\objects,
	// $game_meshes$ -> <project>\gamedata\meshes. `rel` contributes its folder
	// part, so the asset keeps its place in the tree.
	ECORE_API bool	ProjectMirrorDir(LPCSTR fs_alias, LPCSTR rel, string_path& out, xr_string& err);
	// Copies one file out of the shared SDK library (the "Editor Content" source)
	// into the project, at the mirrored location above. Returns false when the
	// source is simply absent, which is the normal answer while probing optional
	// companions (a .thm beside a texture, the .omf beside an .ogf) - only real
	// errors are recorded in the report.
	ECORE_API bool	CopyLibraryFile	(LPCSTR fs_alias, LPCSTR rel, bool overwrite, SReport& rep);

	// MCP command handlers: fill the whole JSON response; raw = request line.
	// 'src' / 'path' accept a ';'-separated list.
	ECORE_API void	McpCopy		(LPCSTR raw, xr_string& out);
	ECORE_API void	McpMove		(LPCSTR raw, xr_string& out);
	ECORE_API void	McpDelete	(LPCSTR raw, xr_string& out);
}
