#include "stdafx.h"
#include "EditorGameContent.h"
#include "EditorProject.h"
#include "XFinedMCP.h"
#include "../../../XrCore/LocatorAPI.h"

//------------------------------------------------------------------------------
// state
//------------------------------------------------------------------------------
// Virtual prefix every archived entry is registered under inside the private
// VFS. It only has to be unique within that VFS - which holds nothing but the
// linked game's archives - so it stays short and keeps the 256-byte name limit
// of CLocatorAPI::Register well out of the picture.
static const char*		kPrefix		= "darf\\";

static CLocatorAPI*				s_VFS		= 0;
static char						s_Root[MAX_PATH] = {};
static xr_vector<EditorGameContent::SItem>	s_Items;

//------------------------------------------------------------------------------
// helpers
//------------------------------------------------------------------------------
static bool DirExists(const char* p)
{
	const DWORD a = ::GetFileAttributesA(p);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool FileExists(const char* p)
{
	const DWORD a = ::GetFileAttributesA(p);
	return a != INVALID_FILE_ATTRIBUTES && 0 == (a & FILE_ATTRIBUTE_DIRECTORY);
}

// "<root>\a\b" -> creates every missing level; the leaf is a FILE path
static void EnsureDirsFor(const char* file_path)
{
	char acc[MAX_PATH] = {};
	const char* last = strrchr(file_path, '\\');
	if (!last) return;
	for (const char* p = file_path; p < last; ++p)
	{
		if (*p == '\\' && acc[0] && acc[xr_strlen(acc) - 1] != ':')
			::CreateDirectoryA(acc, NULL);
		acc[p - file_path]		= *p;
		acc[p - file_path + 1]	= 0;
	}
	if (acc[0]) ::CreateDirectoryA(acc, NULL);
}

static bool ItemLess(const EditorGameContent::SItem& a, const EditorGameContent::SItem& b)
{
	return a.path < b.path;
}

//------------------------------------------------------------------------------
// validation
//------------------------------------------------------------------------------
bool EditorGameContent::LooksLikeGame(LPCSTR game_root, xr_string& reason)
{
	reason = "";
	if (!game_root || !game_root[0])	{ reason = "no folder selected";				return false; }
	if (!DirExists(game_root))			{ reason = "folder does not exist";				return false; }

	char p[MAX_PATH];
	sprintf_s(p, "%s\\fsgame.ltx", game_root);
	if (!FileExists(p))					{ reason = "no fsgame.ltx - not an X-Ray game folder"; return false; }

	char db[MAX_PATH], gd[MAX_PATH];
	sprintf_s(db, "%s\\database", game_root);
	sprintf_s(gd, "%s\\gamedata", game_root);
	if (!DirExists(db) && !DirExists(gd)){ reason = "neither database\\ nor gamedata\\ is present"; return false; }

	return true;
}

//------------------------------------------------------------------------------
// mount
//------------------------------------------------------------------------------
// <root>\database\*.xdb* / *.db*, alphabetical so the load order (and thus
// which archive wins a duplicate path) matches what the game itself does
static void CollectArchives(const char* root, xr_vector<xr_string>& out)
{
	char mask[MAX_PATH];
	sprintf_s(mask, "%s\\database\\*", root);
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		if (!fd.nFileSizeLow && !fd.nFileSizeHigh) continue;		// empty file, never an archive
		const char* ext = strrchr(fd.cFileName, '.');
		if (!ext) continue;
		// .db / .db0.. / .xdb / .xdb0..
		if (0 != _strnicmp(ext, ".db", 3) && 0 != _strnicmp(ext, ".xdb", 4)) continue;
		char full[MAX_PATH];
		sprintf_s(full, "%s\\database\\%s", root, fd.cFileName);
		out.push_back(xr_string(full));
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
	std::sort(out.begin(), out.end());
}

// loose <root>\gamedata\** ; `rel` is the path accumulated so far
static void ScanLoose(const char* base, const char* rel, xr_vector<EditorGameContent::SItem>& out)
{
	char mask[MAX_PATH];
	sprintf_s(mask, "%s\\%s%s*", base, rel, rel[0] ? "\\" : "");
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		char sub[MAX_PATH];
		sprintf_s(sub, "%s%s%s", rel, rel[0] ? "\\" : "", fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			ScanLoose(base, sub, out);
		}
		else
		{
			_strlwr_s(sub, sizeof(sub));
			EditorGameContent::SItem it;
			it.path		= sub;
			it.size		= fd.nFileSizeLow;
			it.source	= EditorGameContent::srcLoose;
			out.push_back(it);
		}
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
}

bool EditorGameContent::Mount(LPCSTR game_root, xr_string& err)
{
	err = "";
	if (!LooksLikeGame(game_root, err)) return false;

	if (s_VFS && 0 == _stricmp(s_Root, game_root)) return true;	// already up
	Unmount();

	xr_strcpy(s_Root, sizeof(s_Root), game_root);
	s_VFS = xrFS_CreateArchiveVFS();

	// ---- packed content ---------------------------------------------------
	xr_vector<xr_string> archives;
	CollectArchives(s_Root, archives);
	for (u32 i = 0; i < archives.size(); ++i)
	{
		// read-only mount under the private prefix; the archive's own
		// [header] entry_point ("gamedata") is deliberately ignored
		if (!s_VFS->ProcessArchiveAs(archives[i].c_str(), kPrefix))
			Msg("! DARF: '%s' is not a readable archive, skipped", archives[i].c_str());
	}

	FS_FileSet packed;
	s_VFS->file_list_prefix(packed, kPrefix);
	for (FS_FileSetIt it = packed.begin(); it != packed.end(); ++it)
	{
		SItem item;
		item.path	= it->name;
		item.size	= (u32)it->size;
		item.source	= srcArchive;
		s_Items.push_back(item);
	}

	// ---- loose overrides --------------------------------------------------
	char gd[MAX_PATH];
	sprintf_s(gd, "%s\\gamedata", s_Root);
	if (DirExists(gd)) ScanLoose(gd, "", s_Items);

	// loose beats packed on a duplicate path, exactly like the game resolves it
	std::sort(s_Items.begin(), s_Items.end(), ItemLess);
	{
		xr_vector<SItem> merged;
		merged.reserve(s_Items.size());
		for (u32 i = 0; i < s_Items.size(); ++i)
		{
			if (!merged.empty() && merged.back().path == s_Items[i].path)
			{
				if (s_Items[i].source == srcLoose) merged.back() = s_Items[i];
				continue;
			}
			merged.push_back(s_Items[i]);
		}
		s_Items.swap(merged);
	}

	Msg("* DARF content mounted: %d archives, %d files (%s)",
		(int)archives.size(), (int)s_Items.size(), s_Root);
	return true;
}

void EditorGameContent::Unmount()
{
	s_Items.clear();
	if (s_VFS) xrFS_DestroyArchiveVFS(s_VFS);
	s_VFS = 0;
	s_Root[0] = 0;
}

bool	EditorGameContent::Mounted()		{ return !!s_VFS; }
LPCSTR	EditorGameContent::MountedRoot()	{ return s_Root; }
int		EditorGameContent::Count()			{ return (int)s_Items.size(); }

const EditorGameContent::SItem* EditorGameContent::Get(int index)
{
	return (index >= 0 && index < (int)s_Items.size()) ? &s_Items[index] : 0;
}

bool EditorGameContent::EnsureMounted(xr_string& err)
{
	err = "";
	if (!EditorProject::Active())	{ err = "no project is open";	return false; }
	if (!EditorProject::GameLinked())
	{
		err = "the project is not linked to a game install";
		return false;
	}
	if (s_VFS && 0 == _stricmp(s_Root, EditorProject::GameRoot())) return true;
	return Mount(EditorProject::GameRoot(), err);
}

int EditorGameContent::Find(LPCSTR rel_path)
{
	if (!rel_path || !rel_path[0]) return -1;
	char low[MAX_PATH];
	strncpy_s(low, sizeof(low), rel_path, _TRUNCATE);
	for (char* p = low; *p; ++p) if (*p == '/') *p = '\\';
	_strlwr_s(low, sizeof(low));

	SItem key; key.path = low; key.size = 0; key.source = srcArchive;
	xr_vector<SItem>::iterator it = std::lower_bound(s_Items.begin(), s_Items.end(), key, ItemLess);
	if (it == s_Items.end() || it->path != key.path) return -1;
	return (int)(it - s_Items.begin());
}

//------------------------------------------------------------------------------
// reading - always read-only, never a write handle on the game folder
//------------------------------------------------------------------------------
u8* EditorGameContent::ReadBytes(int index, u32& size)
{
	size = 0;
	const SItem* it = Get(index);
	if (!it) return 0;

	if (it->source == srcArchive)
	{
		if (!s_VFS) return 0;
		string_path vp;
		sprintf_s(vp, "%s%s", kPrefix, it->path.c_str());
		IReader* R = s_VFS->r_open(0, vp);
		if (!R) return 0;
		const u32 len = (u32)R->length();
		u8* data = 0;
		if (len)
		{
			data = xr_alloc<u8>(len);
			R->seek(0);
			R->r(data, len);
		}
		s_VFS->r_close(R);
		size = len;
		return data;
	}

	string_path real;
	sprintf_s(real, "%s\\gamedata\\%s", s_Root, it->path.c_str());
	HANDLE h = ::CreateFileA(real, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
							 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return 0;
	const DWORD len = ::GetFileSize(h, NULL);
	u8* data = 0;
	if (len && len != INVALID_FILE_SIZE)
	{
		data = xr_alloc<u8>(len);
		DWORD got = 0;
		if (!::ReadFile(h, data, len, &got, NULL) || got != len)
		{
			xr_free(data);
			data = 0;
		}
		else size = len;
	}
	::CloseHandle(h);
	return data;
}

void EditorGameContent::FreeBytes(u8* data)
{
	if (data) xr_free(data);
}

//------------------------------------------------------------------------------
// the ONE write path: <project>\gamedata\<rel>. Nothing here can name a
// destination inside the game folder.
//------------------------------------------------------------------------------
bool EditorGameContent::CopyToProject(LPCSTR rel_path, bool overwrite,
									  string_path& target, bool& existed, xr_string& err)
{
	target[0]	= 0;
	existed		= false;
	err			= "";

	if (!EditorProject::Active())	{ err = "no project is open";	return false; }

	const int idx = Find(rel_path);
	if (idx < 0)					{ err = "unknown file";			return false; }

	sprintf_s(target, "%s\\gamedata\\%s", EditorProject::Root(), s_Items[idx].path.c_str());
	if (FileExists(target))
	{
		existed = true;
		if (!overwrite) { err = "already exists"; return false; }
	}

	u32 size = 0;
	u8* data = ReadBytes(idx, size);
	if (!data && size)				{ err = "can't read the source file"; return false; }

	EnsureDirsFor(target);
	HANDLE h = ::CreateFileA(target, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		FreeBytes(data);
		err = "can't create the target file";
		return false;
	}
	bool ok = true;
	if (size)
	{
		DWORD written = 0;
		ok = !!::WriteFile(h, data, size, &written, NULL) && written == size;
	}
	::CloseHandle(h);
	FreeBytes(data);
	if (!ok) err = "write failed";
	return ok;
}

//------------------------------------------------------------------------------
// placement
//------------------------------------------------------------------------------
bool EditorGameContent::ResolvePlaceable(LPCSTR rel_path, string_path& ref)
{
	ref[0] = 0;
	if (!rel_path || !rel_path[0]) return false;

	// only .object references can become an OBJCLASS_SCENEOBJECT, and only if
	// the editor's own object library can actually load them
	const char* ext = strrchr(rel_path, '.');
	if (!ext || 0 != _stricmp(ext, ".object")) return false;

	string_path name;
	strncpy_s(name, sizeof(name), rel_path, _TRUNCATE);
	if (char* dot = strrchr(name, '.')) *dot = 0;

	string_path fn;
	if (!FS.exist(fn, _objects_, rel_path)) return false;

	xr_strcpy(ref, sizeof(ref), name);
	return true;
}

//------------------------------------------------------------------------------
// MCP
//------------------------------------------------------------------------------
static void JsonAppendPath(xr_string& out, LPCSTR s)
{
	// backslashes travel as forward slashes, same as the core MCP layer
	for (const char* p = s; *p; ++p)
	{
		if (*p == '"') { out += "'"; continue; }
		out += (*p == '\\') ? '/' : *p;
	}
}

static int ArgInt(LPCSTR raw, LPCSTR field, int def)
{
	char pat[64];
	sprintf_s(pat, "\"%s\"", field);
	const char* k = raw ? strstr(raw, pat) : 0;
	if (!k) return def;
	const char* c = strchr(k + xr_strlen(pat), ':');
	if (!c) return def;
	const int v = atoi(c + 1);
	return v > 0 ? v : def;
}

static bool ArgBool(LPCSTR raw, LPCSTR field, bool def)
{
	char pat[64];
	sprintf_s(pat, "\"%s\"", field);
	const char* k = raw ? strstr(raw, pat) : 0;
	if (!k) return def;
	const char* c = strchr(k + xr_strlen(pat), ':');
	if (!c) return def;
	while (*++c == ' ') {}
	return 0 == strncmp(c, "true", 4);
}

void EditorGameContent::McpList(LPCSTR raw, xr_string& out)
{
	xr_string err;
	if (!EnsureMounted(err))
	{
		out = "{\"ok\":false,\"error\":\"";
		out += err;
		out += "\"}";
		return;
	}

	char filter[256] = {}, ext[64] = {};
	XFinedMCP::GetArg(raw, "filter", filter, sizeof(filter));
	XFinedMCP::GetArg(raw, "ext", ext, sizeof(ext));
	_strlwr_s(filter, sizeof(filter));
	_strlwr_s(ext, sizeof(ext));
	const int limit = ArgInt(raw, "limit", 200);

	int total = 0, shown = 0;
	xr_string list;
	for (u32 i = 0; i < s_Items.size(); ++i)
	{
		const SItem& it = s_Items[i];
		if (filter[0] && !strstr(it.path.c_str(), filter)) continue;
		if (ext[0])
		{
			const char* e = strrchr(it.path.c_str(), '.');
			if (!e) continue;
			if (0 != _stricmp(e + 1, ext[0] == '.' ? ext + 1 : ext)) continue;
		}
		++total;
		if (shown >= limit) continue;
		if (shown) list += ",";
		list += "{\"path\":\"";
		JsonAppendPath(list, it.path.c_str());
		char tail[64];
		sprintf_s(tail, "\",\"size\":%u,\"source\":\"%s\"}", it.size,
			it.source == srcArchive ? "archive" : "loose");
		list += tail;
		++shown;
	}

	char head[128];
	sprintf_s(head, "{\"ok\":true,\"shown\":%d,\"total\":%d,\"files\":[", shown, total);
	out = head;
	out += list;
	out += "]}";
}

void EditorGameContent::McpCopy(LPCSTR raw, xr_string& out)
{
	xr_string err;
	if (!EnsureMounted(err))
	{
		out = "{\"ok\":false,\"error\":\"";
		out += err;
		out += "\"}";
		return;
	}

	char path[MAX_PATH] = {};
	if (!XFinedMCP::GetArg(raw, "path", path, sizeof(path)) || !path[0])
	{
		out = "{\"ok\":false,\"error\":\"missing 'path' argument\"}";
		return;
	}
	const bool overwrite = ArgBool(raw, "overwrite", false);

	string_path target;
	bool existed = false;
	const bool ok = CopyToProject(path, overwrite, target, existed, err);

	out = "{\"ok\":";
	out += ok ? "true" : "false";
	out += ",\"target\":\"";
	JsonAppendPath(out, target);
	out += "\",\"copied\":";
	out += ok ? "true" : "false";
	out += ",\"exists\":";
	out += existed ? "true" : "false";
	if (!ok)
	{
		out += ",\"error\":\"";
		out += err;
		out += "\"";
	}
	out += "}";
}
