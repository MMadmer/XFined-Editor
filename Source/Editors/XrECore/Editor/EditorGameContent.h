#pragma once

// Read-only view of the game install a project is linked to (DARF = Dead Air
// Refined and any other X-Ray build laid out the same way).
//
// The game keeps its content PACKED in <game>\database\*.xdb* plus a small
// loose <game>\gamedata\ override tree. Both are exposed here as one flat,
// sorted list of gamedata-relative paths ("meshes\\foo.ogf"), which the DARF
// Content browser and the darf_* MCP commands turn back into a folder tree.
// A loose file shadows the archived one with the same path, exactly like the
// game itself resolves them.
//
// Mounting: the editor's process-wide FS is an ELocatorAPI, which has no
// archive support at all, so the archives go into a SEPARATE, private
// CLocatorAPI (xrFS_CreateArchiveVFS) that is never _initialize()d - no path
// aliases, no disk scan - with every entry registered under the virtual prefix
// "darf\" through CLocatorAPI::ProcessArchiveAs. Because it is its own
// registry, nothing mounted here is even visible to the editor's FS, let alone
// able to shadow an SDK asset. The one and only call this unit makes into the
// shared FS is a read-only FS.exist() in ResolvePlaceable.
//
// READ ONLY: this unit opens the game install for reading and nothing else.
// There is no write/rename/delete/create entry point that takes a path under
// the game root - the single writing function, CopyToProject, only ever writes
// below the project's gamedata folder.

namespace EditorGameContent
{
	enum ESource
	{
		srcArchive	= 0,	// lives inside a database\*.xdb* archive
		srcLoose	= 1,	// loose file under the game's gamedata folder
	};

	struct SItem
	{
		xr_string			path;		// gamedata-relative, '\\'-separated, lowercase
		u32					size;
		ESource				source;
	};

	// folder checks used by the project link validator and by Mount itself
	ECORE_API bool	LooksLikeGame	(LPCSTR game_root, xr_string& reason);

	// mounts <root>\database\*.xdb* and indexes <root>\gamedata\**; idempotent
	ECORE_API bool	Mount			(LPCSTR game_root, xr_string& err);
	// drops the item list (the FS mount itself is permanent for the session)
	ECORE_API void	Unmount			();
	ECORE_API bool	Mounted			();
	ECORE_API LPCSTR MountedRoot	();
	// mounts the active project's linked game on first use
	ECORE_API bool	EnsureMounted	(xr_string& err);

	ECORE_API int	Count			();
	ECORE_API const SItem* Get		(int index);
	ECORE_API int	Find			(LPCSTR rel_path);		// -1 when unknown

	// reads a whole item into memory; free with FreeBytes. Archived items go
	// through FS.r_open on the virtual path, loose ones through a shared-read
	// handle on the real file. Returns 0 on any failure, never throws.
	ECORE_API u8*	ReadBytes		(int index, u32& size);
	ECORE_API void	FreeBytes		(u8* data);

	// copies an item to <project>\gamedata\<rel_path>, creating directories.
	// existed=true + false return when the target is there and !overwrite.
	ECORE_API bool	CopyToProject	(LPCSTR rel_path, bool overwrite,
									 string_path& target, bool& existed, xr_string& err);

	// item -> library object name for COMMAND_CB_PLACE_ASSET; false when the
	// item is not a reference the object library can resolve
	ECORE_API bool	ResolvePlaceable(LPCSTR rel_path, string_path& ref);

	// MCP command handlers: fill the full JSON response; raw = request line
	ECORE_API void	McpList			(LPCSTR raw, xr_string& out);
	ECORE_API void	McpCopy			(LPCSTR raw, xr_string& out);

	// shared JSON value emitter: backslashes travel as forward slashes, quotes
	// fold to apostrophes. Used by every MCP responder in this layer so the
	// escaping rules stay in one place.
	ECORE_API void	JsonAppendPath	(xr_string& out, LPCSTR s);
}
