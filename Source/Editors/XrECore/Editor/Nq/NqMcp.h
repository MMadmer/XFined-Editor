#pragma once

// NQ - MCP commands quest_* (docs/NQ_ARCHITECTURE.md par. 13.11). Called from the
// editor-layer inspector; every command answers a complete JSON object and
// never shows a dialog.

namespace NqMcp
{
	// true when `cmd` was a quest command (response filled)
	ECORE_API bool	Handle	(LPCSTR cmd, LPCSTR raw, xr_string& out);
}
