#pragma once

// NQ - reading and writing .nqasset files (docs/NQ_ARCHITECTURE.md par. 4, par. 13.3).
//
// Reading goes through the LuaJIT C API exactly like the game does: the chunk
// runs in an EMPTY environment (no libraries, no globals), so anything that is
// not plain data fails to load and is reported with the Lua line. Writing is a
// canonical serializer: fixed key order, fixed layout, generated header
// comment with the quest outline; write(read(x)) is idempotent.

#include "NqAsset.h"

namespace NqLua
{
	struct SError
	{
		xr_string	message;		// UTF-8, without the chunk name prefix
		int			line;			// 1-based, 0 = unknown
		int			col;			// 1-based, 0 = unknown (best effort from "near '...'")
		SError() : line(0), col(0) {}
	};

	// -- reading ------------------------------------------------------------
	// runs `text` as a chunk in an empty environment and converts the value it
	// returns. False + err on syntax errors, runtime errors (code outside
	// strings), execution limit, or when nothing/a non-table is returned.
	ECORE_API bool	ParseValue	(LPCSTR text, u32 len, LPCSTR chunk_name, SNqValue& out, SError& err);
	// generic value -> typed quest. Shape problems (nodes not a table, node id
	// missing, ...) are collected as messages, the rest is converted anyway so
	// the author sees as much of the quest as can be read.
	ECORE_API void	QuestFromValue	(const SNqValue& v, SNqQuest& q, xr_vector<xr_string>& shape_errors);
	ECORE_API bool	NodeFromValue	(const SNqValue& v, SNqNode& n, xr_vector<xr_string>& shape_errors);
	ECORE_API bool	ActionFromValue	(const SNqValue& v, SNqAction& a, xr_string& err);
	ECORE_API bool	CondFromValue	(const SNqValue& v, SNqCond& c, xr_string& err);
	ECORE_API void	TaskFromValue	(LPCSTR id, const SNqValue& v, SNqTask& t);
	// orders a node's out pins by the catalog (case pins in case order, then
	// the declared pins, unknown pins last alphabetically); NodeFromValue and
	// NqDoc call it so the model, the writer and the outline agree
	ECORE_API void	CanonicalizePins(SNqNode& n);
	ECORE_API void	CanonicalizePins(SNqQuest& q);
	// ParseValue + QuestFromValue; shape errors are appended to err.message
	// (one per line) and make the call fail
	ECORE_API bool	ParseQuest	(LPCSTR text, u32 len, LPCSTR chunk_name, SNqQuest& q, SError& err);
	// compiles `code` (custom Lua of a `lua` action/condition) without running
	// it. False + err (line/col relative to the code) on a syntax error.
	ECORE_API bool	SyntaxCheck	(LPCSTR code, SError& err);

	// -- writing --------------------------------------------------------------
	// canonical file text (UTF-8, '\n', header comment + return { ... })
	ECORE_API xr_string	Write		(const SNqQuest& q);
	// generic serializer for any value (used for MCP echo and clipboard):
	// `indent` = current indent in spaces
	ECORE_API xr_string	WriteValue	(const SNqValue& v, int indent = 0);
	// one node as a Lua table literal (clipboard fragments)
	ECORE_API xr_string	WriteNode	(const SNqNode& n, int indent = 0);
	// typed -> generic (the exact tables the writer emits)
	ECORE_API void	NodeToValue	(const SNqNode& n, SNqValue& out);
	ECORE_API void	ActionToValue(const SNqAction& a, SNqValue& out);
	ECORE_API void	CondToValue	(const SNqCond& c, SNqValue& out);
	ECORE_API void	QuestToValue(const SNqQuest& q, SNqValue& out);

	// text outline of the graph (par. 4.5 p.5), one line per node, no problems
	// line; the same function feeds the file header, quest_get and quest_list
	ECORE_API xr_string	Outline		(const SNqQuest& q);
	// one outline line for a node
	ECORE_API xr_string	OutlineNode	(const SNqQuest& q, const SNqNode& n);

	// -- files ------------------------------------------------------------------
	ECORE_API bool	ReadFile	(LPCSTR path, xr_string& text, xr_string& err);
	// writes bytes as is (no BOM, no newline translation); creates directories
	ECORE_API bool	WriteFile	(LPCSTR path, const xr_string& text, xr_string& err);
	// last-write time (0 when the file does not exist)
	ECORE_API u64	FileTime	(LPCSTR path);
}
