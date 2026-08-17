#pragma once

namespace CommandPalette
{
struct SResult
{
	xr_string id;
	xr_string category;
	xr_string path;
	xr_string name;
	xr_string shortcut;
	u32 command{};
	u32 subcommand{};
	int score{};
};

ECORE_API void Open(LPCSTR query = "");
ECORE_API void Close();
ECORE_API bool IsOpen();
ECORE_API void Draw();
ECORE_API void Query(LPCSTR query, xr_vector<SResult>& results, u32 limit = 100);
ECORE_API bool Execute(LPCSTR id);
ECORE_API bool Execute(u32 command, u32 subcommand);
}
