#pragma once

// NQ - small shared helpers (encoding, JSON emitting, file walking, paths).
// Kept out of the model header so the Phase 4 UI can include only what it uses.

namespace NqUtil
{
	// -- encoding ---------------------------------------------------------------
	ECORE_API xr_string	Cp1251ToUtf8	(LPCSTR src);
	ECORE_API xr_string	Utf8ToCp1251	(LPCSTR src);
	// true when every character of the UTF-8 text exists in cp1251; `bad`
	// receives the offending characters (UTF-8, deduplicated, at most a few)
	ECORE_API bool		Cp1251Covers	(const xr_string& utf8, xr_string& bad);
	// bytes -> UTF-8 text: BOM-less UTF-8 passes through, otherwise cp1251
	ECORE_API xr_string	GameTextToUtf8	(const char* bytes, u32 size);
	// first `max_bytes` of a UTF-8 string without cutting a multibyte sequence;
	// appends "..." when clipped
	ECORE_API xr_string	ClipUtf8		(const xr_string& s, u32 max_bytes);

	// -- json -------------------------------------------------------------------
	// appends the escaped string body (no quotes) - quotes, backslashes and
	// control characters escaped, so the one-line MCP protocol never breaks
	ECORE_API void		JsonAppend		(xr_string& out, LPCSTR s);
	ECORE_API void		JsonAppend		(xr_string& out, const xr_string& s);
	// "..." with escaping
	ECORE_API void		JsonString		(xr_string& out, LPCSTR s);
	ECORE_API void		JsonString		(xr_string& out, const xr_string& s);
	// path with forward slashes, quoted
	ECORE_API void		JsonPath		(xr_string& out, LPCSTR s);
	ECORE_API void		JsonNumber		(xr_string& out, double n);
	ECORE_API void		JsonInt			(xr_string& out, int n);
	ECORE_API void		JsonBool		(xr_string& out, bool b);
	// ["a","b",...]
	ECORE_API void		JsonStringArray	(xr_string& out, const xr_vector<xr_string>& v);
	// {"ok":false,"error":"..."}
	ECORE_API void		JsonError		(xr_string& out, LPCSTR message);
	ECORE_API void		JsonError		(xr_string& out, const xr_string& message);

	// -- request arguments (raw MCP request line) --------------------------------
	// XFinedMCP::GetArg reads quoted strings; these read bare numbers/bools
	ECORE_API bool		ArgBool			(LPCSTR raw, LPCSTR field, bool def);
	ECORE_API int		ArgInt			(LPCSTR raw, LPCSTR field, int def);
	ECORE_API bool		ArgFloat		(LPCSTR raw, LPCSTR field, float& out);
	// string argument of any length (GetArg needs a fixed buffer)
	ECORE_API bool		ArgString		(LPCSTR raw, LPCSTR field, xr_string& out);

	// -- paths and files ----------------------------------------------------------
	// SDK root (the folder holding fs.ltx and the bundled gamedata), no trailing slash
	ECORE_API LPCSTR	SdkRoot			();
	// <sdk>\_appdata_ (created on demand), no trailing slash
	ECORE_API LPCSTR	AppDataRoot		();
	ECORE_API bool		FileExists		(LPCSTR path);
	ECORE_API bool		DirExists		(LPCSTR path);
	// backslashes, no trailing slash, no "\.\"; keeps case
	ECORE_API void		NormalizePath	(LPCSTR in, xr_string& out);
	// lowercase normalized absolute path - registry key for documents
	ECORE_API xr_string	PathKey			(LPCSTR path);
	// resolves a path given relative to the project root (absolute paths pass
	// through); false when no project is active. Does NOT check existence.
	ECORE_API bool		ProjectPath		(LPCSTR rel_or_abs, xr_string& out, xr_string& err);
	// path relative to the project root with forward slashes, or the input
	ECORE_API xr_string	ProjectRelative	(LPCSTR abs);
	// recursive file listing by extension (".nqasset"), absolute paths, sorted;
	// `skip_root_entry` is asked for every top-level entry (editor-only folders)
	typedef bool (*TSkipRootEntry)(LPCSTR name);
	ECORE_API void		ListFiles		(LPCSTR root, LPCSTR ext, xr_vector<xr_string>& out, TSkipRootEntry skip = 0);
	// file name without directory and extension
	ECORE_API xr_string	BaseName		(LPCSTR path);
	ECORE_API xr_string	Extension		(LPCSTR path);		// ".nqasset" (lowercase)

	// -- misc ---------------------------------------------------------------------
	ECORE_API void		Split			(LPCSTR s, char sep, xr_vector<xr_string>& out);	// trimmed, empties dropped
	ECORE_API xr_string	Trim			(const xr_string& s);
	ECORE_API u32		Fnv1a			(LPCSTR s);
	ECORE_API xr_string	Format			(LPCSTR fmt, ...);
}
