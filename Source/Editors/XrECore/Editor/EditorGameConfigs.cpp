#include "stdafx.h"
#include "EditorGameConfigs.h"
#include "EditorGameContent.h"
#include "EditorProject.h"

namespace
{
static xr_string	s_Fingerprint;		// what pSettings currently describes
static string_path	s_SavedRoot = {};	// $game_config$ before the first swap
static bool			s_SavedRootKept = false;

// <editor root>\_appdata_\game_configs\ - one cache, refilled per fingerprint.
// The exe lives in Bin\x64\<cfg>\, so the root is found by walking up to the
// folder that holds fs.ltx - same place _projects.ini and _appdata_ live.
void CacheRoot(string_path& dst)
{
	::GetModuleFileNameA(NULL, dst, sizeof(dst));
	if (char* p = strrchr(dst, '\\')) *p = 0;
	for (int up = 0; up < 6; ++up)
	{
		string_path probe;
		sprintf_s(probe, "%s\\fs.ltx", dst);
		if (INVALID_FILE_ATTRIBUTES != ::GetFileAttributesA(probe)) break;
		char* cut = strrchr(dst, '\\');
		if (!cut) break;
		*cut = 0;
	}
	xr_strcat(dst, sizeof(dst), "\\_appdata_\\game_configs");
}

void WipeTreeRec(LPCSTR dir)
{
	WIN32_FIND_DATAA fd;
	string_path mask;
	sprintf_s(mask, "%s\\*", dir);
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (INVALID_FILE_ATTRIBUTES == ::GetFileAttributesA(dir)) return;
	if (INVALID_HANDLE_VALUE == h) return;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		string_path p;
		sprintf_s(p, "%s\\%s", dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			WipeTreeRec(p);
			::RemoveDirectoryA(p);
		}
		else
		{
			::SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
			::DeleteFileA(p);
		}
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
}

// creates every directory on the way to `file_path` - the FILE itself is not
// touched (creating it as a directory would break the CreateFile that follows)
void MakeDirChain(LPCSTR file_path)
{
	string_path acc = {};
	for (const char* p = file_path; *p; ++p)
	{
		if (*p == '\\')
		{
			if (acc[0] && acc[xr_strlen(acc) - 1] != ':') ::CreateDirectoryA(acc, NULL);
		}
		acc[p - file_path] = *p;
		acc[p - file_path + 1] = 0;
	}
}
} // namespace

LPCSTR EditorGameConfigs::ActiveFingerprint() { return s_Fingerprint.c_str(); }

bool EditorGameConfigs::Activate(xr_string& err)
{
	err.clear();

	xr_string mount_err;
	if (!EditorGameContent::EnsureMounted(mount_err))
	{
		err = mount_err.empty() ? xr_string("no game linked") : mount_err;
		return false;
	}

	// ---- fingerprint: the game root plus what its configs add up to ---------
	int   files = 0;
	u64   bytes = 0;
	const int total = EditorGameContent::Count();
	for (int i = 0; i < total; ++i)
	{
		const EditorGameContent::SItem* it = EditorGameContent::Get(i);
		if (!it || 0 != _strnicmp(it->path.c_str(), "configs\\", 8)) continue;
		++files;
		bytes += it->size;
	}
	if (!files) { err = "the linked game has no configs"; return false; }

	char fp[512];
	sprintf_s(fp, "%s|%d files|%llu bytes", EditorGameContent::MountedRoot(), files, bytes);
	if (s_Fingerprint == fp) return true;			// already active

	string_path cache;
	CacheRoot(cache);

	// ---- reuse or rebuild the extracted tree --------------------------------
	string_path marker;
	sprintf_s(marker, "%s\\.fingerprint", cache);
	bool cached = false;
	if (HANDLE h = ::CreateFileA(marker, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		INVALID_HANDLE_VALUE != h)
	{
		char buf[512] = {};
		DWORD rd = 0;
		::ReadFile(h, buf, sizeof(buf) - 1, &rd, NULL);
		::CloseHandle(h);
		cached = (0 == strcmp(buf, fp));
	}

	if (!cached)
	{
		Msg("* game configs: extracting %d file(s) from '%s'...", files, EditorGameContent::MountedRoot());
		WipeTreeRec(cache);

		int written = 0;
		for (int i = 0; i < total; ++i)
		{
			const EditorGameContent::SItem* it = EditorGameContent::Get(i);
			if (!it || 0 != _strnicmp(it->path.c_str(), "configs\\", 8)) continue;

			u32 sz = 0;
			u8* data = EditorGameContent::ReadBytes(i, sz);
			if (!data) { Msg("! game configs: can't read '%s', skipped", it->path.c_str()); continue; }

			string_path dst;
			// the path already starts with configs\ - the cache root replaces
			// the gamedata root, so system.ltx lands at <cache>\system.ltx
			sprintf_s(dst, "%s\\%s", cache, it->path.c_str() + 8);
			MakeDirChain(dst);
			if (HANDLE f = ::CreateFileA(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
				INVALID_HANDLE_VALUE != f)
			{
				DWORD wr = 0;
				::WriteFile(f, data, sz, &wr, NULL);
				::CloseHandle(f);
				if (wr == sz) ++written;
			}
			EditorGameContent::FreeBytes(data);
		}

		if (HANDLE f = ::CreateFileA(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			INVALID_HANDLE_VALUE != f)
		{
			DWORD wr = 0;
			::WriteFile(f, fp, DWORD(xr_strlen(fp)), &wr, NULL);
			::CloseHandle(f);
		}
		Msg("* game configs: %d file(s) extracted to %s", written, cache);
	}

	// ---- repoint $game_config$ and rebuild pSettings ------------------------
	FS_Path* P = FS.get_path("$game_config$");
	if (!P) { err = "no $game_config$ alias in the editor fs"; return false; }
	if (!s_SavedRootKept) { xr_strcpy(s_SavedRoot, P->m_Path); s_SavedRootKept = true; }
	string_path root;
	sprintf_s(root, "%s\\", cache);
	P->_set_root(root);
	P->_set("");

	string_path system_ltx;
	sprintf_s(system_ltx, "%s\\system.ltx", cache);
	if (INVALID_FILE_ATTRIBUTES == ::GetFileAttributesA(system_ltx))
	{
		err = "the linked game has no configs\\system.ltx";
		return false;
	}

	// the factory normally owns this global; the hook that called us recreates
	// the factory right after, and its initialize() reads the repointed alias.
	// Dropping the old ini here keeps the window where both exist at zero.
	if (pSettings)
	{
		CInifile** s = (CInifile**)(&pSettings);
		xr_delete(*s);
	}
	pSettings = xr_new<CInifile>(system_ltx, TRUE);

	s_Fingerprint = fp;
	Msg("* game configs: pSettings now describes '%s' (%d sections)",
		EditorGameContent::MountedRoot(), int(pSettings->sections().size()));
	return true;
}
