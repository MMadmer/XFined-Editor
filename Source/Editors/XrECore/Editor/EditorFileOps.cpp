#include "stdafx.h"
#include "EditorFileOps.h"
#include "EditorGameContent.h"
#include "EditorProject.h"
#include "XFinedMCP.h"

//------------------------------------------------------------------------------
// small filesystem predicates
//------------------------------------------------------------------------------
static bool DirExists(LPCSTR p)
{
	const DWORD a = ::GetFileAttributesA(p);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool FileExists(LPCSTR p)
{
	const DWORD a = ::GetFileAttributesA(p);
	return a != INVALID_FILE_ATTRIBUTES && 0 == (a & FILE_ATTRIBUTE_DIRECTORY);
}

// "<dir>\<leaf>" with an honest overflow answer: sprintf_s would just blow up on
// a path longer than the buffer, and these are assembled inside deep tree walks
static bool Join(char* dst, u32 dst_size, LPCSTR dir, LPCSTR leaf)
{
	const u32 need = xr_strlen(dir) + 1 + xr_strlen(leaf) + 1;
	if (need > dst_size) return false;
	sprintf_s(dst, dst_size, "%s\\%s", dir, leaf);
	return true;
}

static xr_string Win32Reason(LPCSTR what)
{
	char tmp[128];
	// the numeric code is what actually identifies the failure; a localised
	// FormatMessage sentence would only need escaping on the way out
	sprintf_s(tmp, "%s (win32 %lu)", what, ::GetLastError());
	return xr_string(tmp);
}

//------------------------------------------------------------------------------
// path normalisation — the load-bearing part of the safety contract
//------------------------------------------------------------------------------
static bool IsAbsolute(LPCSTR p)
{
	if (!p || !p[0]) return false;
	if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) return true;	// UNC
	// the separator is required: "D:foo" is DRIVE-RELATIVE (resolved against the
	// per-drive working directory), so treating it as absolute would invent a
	// path the caller never named
	return 0 != isalpha(u8(p[0])) && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}

// Purely textual normalisation, on purpose:
//  * GetFullPathName resolves relative input against the PROCESS CWD, which has
//    nothing to do with the project;
//  * anything canonical (GetFinalPathNameByHandle) needs an open handle, and a
//    copy destination does not exist yet.
// So the segments are walked by hand: '/' becomes '\\', repeats collapse, "."
// disappears and ".." pops. A ".." that would climb above the volume prefix is
// an error rather than a silent clamp - the caller must not be told a path is
// fine when it was quietly rewritten.
static bool LexicalNormalize(LPCSTR in, char* out, u32 out_size)
{
	if (!IsAbsolute(in)) return false;

	char work[MAX_PATH * 2];
	// truncating would normalise a DIFFERENT path than the one asked about
	if (xr_strlen(in) + 1 > sizeof(work)) return false;
	xr_strcpy(work, sizeof(work), in);
	for (char* p = work; *p; ++p) if (*p == '/') *p = '\\';

	// the volume prefix is the floor nothing can climb above
	char		head[MAX_PATH] = {};
	const char*	body = 0;
	if (work[0] == '\\' && work[1] == '\\')
	{
		const char* slash = strchr(work + 2, '\\');			// end of \\server
		const char* share = slash ? slash + 1 : 0;
		if (!share || !share[0]) return false;				// "\\server" alone is not a path
		const char* end = strchr(share, '\\');				// end of \\server\share
		const size_t n = end ? size_t(end - work) : size_t(xr_strlen(work));
		if (n >= sizeof(head)) return false;
		CopyMemory(head, work, n);
		head[n]	= 0;
		body	= end ? end : "";
	}
	else
	{
		head[0]	= work[0];
		head[1]	= ':';
		head[2]	= 0;
		body	= work + 2;
	}

	xr_vector<xr_string>	seg;
	char					token[MAX_PATH];
	u32						t = 0;
	for (const char* p = body; ; ++p)
	{
		if (*p && *p != '\\')
		{
			if (t + 1 >= sizeof(token)) return false;
			token[t++] = *p;
			continue;
		}
		token[t] = 0;
		if (t)
		{
			if (0 == strcmp(token, ".")) {}
			else if (0 == strcmp(token, ".."))
			{
				if (seg.empty()) return false;				// climbed past the volume
				seg.pop_back();
			}
			else
			{
				// ':' would smuggle a drive letter or an alternate data stream
				// past the root check; the rest are simply illegal in a name
				if (strpbrk(token, ":<>\"|?*")) return false;
				seg.push_back(xr_string(token));
			}
		}
		t = 0;
		if (!*p) break;
	}

	xr_string acc = head;
	for (u32 i = 0; i < seg.size(); ++i) { acc += "\\"; acc += seg[i]; }
	if (seg.empty()) acc += "\\";							// bare volume root
	if (acc.size() + 1 > out_size) return false;
	xr_strcpy(out, out_size, acc.c_str());
	return true;
}

// prefix test on two already-normalised paths. The separator check is what stops
// "<root>_backup" from passing as if it were inside "<root>".
static bool IsInside(LPCSTR outer, LPCSTR inner)
{
	const u32 n = xr_strlen(outer);
	if (!n || 0 != _strnicmp(inner, outer, n)) return false;
	if (outer[n - 1] == '\\') return true;					// outer is a volume root
	return inner[n] == 0 || inner[n] == '\\';
}

bool EditorFileOps::ResolveSource(LPCSTR path, string_path& out, xr_string& err)
{
	out[0]	= 0;
	err		= "";
	if (!path || !path[0])			{ err = "empty path";	return false; }

	// a relative path is read against the project root - the process CWD is the
	// SDK folder and would silently point somewhere else entirely
	char joined[MAX_PATH * 2];
	if (IsAbsolute(path))
	{
		// truncating here would hand a DIFFERENT path to the root check, so a
		// path that does not fit is refused instead
		if (xr_strlen(path) + 1 > sizeof(joined))	{ err = "path too long"; return false; }
		xr_strcpy(joined, sizeof(joined), path);
	}
	else
	{
		if (!EditorProject::Active())	{ err = "no project is open"; return false; }
		LPCSTR root = EditorProject::Root();
		if (xr_strlen(root) + 1 + xr_strlen(path) + 1 > sizeof(joined))
		{
			err = "path too long";
			return false;
		}
		sprintf_s(joined, "%s\\%s", root, path);
	}

	if (!LexicalNormalize(joined, out, sizeof(string_path)))
	{
		out[0]	= 0;
		err		= "malformed path";
		return false;
	}
	return true;
}

bool EditorFileOps::ResolveInProject(LPCSTR path, string_path& out, xr_string& err)
{
	if (!ResolveSource(path, out, err)) return false;
	if (!EditorProject::Active())	{ out[0] = 0; err = "no project is open"; return false; }

	string_path root;
	if (!LexicalNormalize(EditorProject::Root(), root, sizeof(root)))
	{
		out[0]	= 0;
		err		= "the project root is not a usable absolute path";
		return false;
	}
	if (!IsInside(root, out))
	{
		out[0]	= 0;
		err		= "path leaves the project folder - only the project is writable";
		return false;
	}
	return true;
}

//------------------------------------------------------------------------------
// reporting
//------------------------------------------------------------------------------
static const u32 kMaxDetails = 64;

static void Note(xr_vector<EditorFileOps::SFailure>& v, LPCSTR path, const xr_string& reason)
{
	if (v.size() >= kMaxDetails) return;
	EditorFileOps::SFailure f;
	f.path		= path ? path : "";
	f.reason	= reason;
	v.push_back(f);
}

static void Fail(EditorFileOps::SReport& rep, LPCSTR path, const xr_string& reason)
{
	++rep.failed;
	Note(rep.failures, path, reason);
}

static void Skip(EditorFileOps::SReport& rep, LPCSTR path, const xr_string& reason)
{
	++rep.skipped;
	Note(rep.skips, path, reason);
}

//------------------------------------------------------------------------------
// primitives
//------------------------------------------------------------------------------
// creates every missing level of a DIRECTORY path
static bool EnsureDir(LPCSTR dir)
{
	if (DirExists(dir)) return true;

	char acc[MAX_PATH];
	xr_strcpy(acc, sizeof(acc), dir);
	for (char* p = acc; *p; ++p)
	{
		if (*p != '\\' || p == acc) continue;
		*p = 0;
		// "D:" and the "\\server\share" halves are not creatable levels; the
		// call simply fails on them and the walk moves on
		if (acc[0] && acc[xr_strlen(acc) - 1] != ':' && !DirExists(acc))
			::CreateDirectoryA(acc, NULL);
		*p = '\\';
	}
	::CreateDirectoryA(acc, NULL);
	return DirExists(dir);
}

static void CopyOneFile(LPCSTR src, LPCSTR dst, bool overwrite, EditorFileOps::SReport& rep)
{
	if (FileExists(dst) && !overwrite)
	{
		Skip(rep, dst, "already exists (pass overwrite=1)");
		return;
	}
	// CopyFile refuses a read-only target, and SDK/game content is read-only
	// far more often than not
	if (overwrite && FileExists(dst))
		::SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);

	if (::CopyFileA(src, dst, overwrite ? FALSE : TRUE))	++rep.files;
	else												Fail(rep, src, Win32Reason("copy failed"));
}

static void CopyTree(LPCSTR src_dir, LPCSTR dst_dir, bool recursive, bool overwrite,
					 EditorFileOps::SReport& rep)
{
	if (!EnsureDir(dst_dir))
	{
		Fail(rep, dst_dir, Win32Reason("cannot create the target folder"));
		return;
	}
	++rep.dirs;

	char mask[MAX_PATH];
	if (!Join(mask, sizeof(mask), src_dir, "*"))
	{
		Fail(rep, src_dir, xr_string("path too long"));
		return;
	}
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		char s[MAX_PATH], d[MAX_PATH];
		if (!Join(s, sizeof(s), src_dir, fd.cFileName) || !Join(d, sizeof(d), dst_dir, fd.cFileName))
		{
			Fail(rep, fd.cFileName, xr_string("path too long"));
			continue;
		}
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			// a junction is not followed: whatever it points at was never
			// checked against the project root
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			{
				Skip(rep, s, "reparse point, not followed");
				continue;
			}
			if (!recursive)
			{
				Skip(rep, s, "sub-folder (pass recursive=1)");
				continue;
			}
			CopyTree(s, d, true, overwrite, rep);
		}
		else
			CopyOneFile(s, d, overwrite, rep);
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
}

static void DeleteTree(LPCSTR dir, EditorFileOps::SReport& rep)
{
	char mask[MAX_PATH];
	if (!Join(mask, sizeof(mask), dir, "*"))
	{
		Fail(rep, dir, xr_string("path too long"));
		return;
	}
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
			char s[MAX_PATH];
			if (!Join(s, sizeof(s), dir, fd.cFileName))
			{
				Fail(rep, fd.cFileName, xr_string("path too long"));
				continue;
			}
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				// removing the link itself is right; recursing through it would
				// delete files that live outside the project
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
				{
					if (::RemoveDirectoryA(s))	++rep.dirs;
					else						Fail(rep, s, Win32Reason("cannot remove the link"));
					continue;
				}
				DeleteTree(s, rep);
			}
			else
			{
				::SetFileAttributesA(s, FILE_ATTRIBUTE_NORMAL);
				if (::DeleteFileA(s))	++rep.files;
				else					Fail(rep, s, Win32Reason("delete failed"));
			}
		} while (::FindNextFileA(h, &fd));
		::FindClose(h);
	}

	::SetFileAttributesA(dir, FILE_ATTRIBUTE_NORMAL);
	if (::RemoveDirectoryA(dir))	++rep.dirs;
	else							Fail(rep, dir, Win32Reason("cannot remove the folder"));
}

// counts what a whole-tree rename is about to move, so the report can still
// give real numbers instead of "1 directory"
static void CountTree(LPCSTR dir, int& files, int& dirs)
{
	++dirs;
	char mask[MAX_PATH];
	if (!Join(mask, sizeof(mask), dir, "*")) return;
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++dirs; continue; }
			char s[MAX_PATH];
			if (Join(s, sizeof(s), dir, fd.cFileName)) CountTree(s, files, dirs);
		}
		else ++files;
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
}

static LPCSTR LeafName(LPCSTR path)
{
	const char* cut = strrchr(path, '\\');
	return cut ? cut + 1 : path;
}

//------------------------------------------------------------------------------
// copy
//------------------------------------------------------------------------------
// last resort for a source that is not on disk: an item inside the linked game's
// archives. Reading DARF and writing into the project is exactly what the
// contract allows, so it stays here rather than becoming a special case upstream.
static bool CopyFromGameArchive(LPCSTR src, LPCSTR dst_dir, bool overwrite, EditorFileOps::SReport& rep)
{
	xr_string mount_err;
	if (!EditorGameContent::EnsureMounted(mount_err)) return false;

	const int idx = EditorGameContent::Find(src);
	if (idx < 0) return false;

	const EditorGameContent::SItem* item = EditorGameContent::Get(idx);
	if (!item) return false;

	char dst[MAX_PATH];
	if (!Join(dst, sizeof(dst), dst_dir, LeafName(item->path.c_str())))
	{
		Fail(rep, src, xr_string("path too long"));
		return true;
	}
	if (FileExists(dst) && !overwrite)
	{
		Skip(rep, dst, "already exists (pass overwrite=1)");
		return true;
	}
	if (!EnsureDir(dst_dir))
	{
		Fail(rep, dst_dir, Win32Reason("cannot create the target folder"));
		return true;
	}

	u32 size = 0;
	u8* data = EditorGameContent::ReadBytes(idx, size);
	// a genuinely empty entry reads back as (0, 0); only a KNOWN-sized item that
	// came back empty is a read failure
	if (!data && item->size)
	{
		Fail(rep, src, xr_string("cannot read the archived file"));
		return true;
	}

	::SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);
	HANDLE h = ::CreateFileA(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		EditorGameContent::FreeBytes(data);
		Fail(rep, dst, Win32Reason("cannot create the target file"));
		return true;
	}
	bool ok = true;
	if (size)
	{
		DWORD written = 0;
		ok = !!::WriteFile(h, data, size, &written, NULL) && written == size;
	}
	::CloseHandle(h);
	EditorGameContent::FreeBytes(data);

	// CloseHandle/FreeBytes already clobbered GetLastError, so no win32 code here
	if (ok)	++rep.files;
	else	Fail(rep, dst, xr_string("write failed"));
	return true;
}

void EditorFileOps::Copy(LPCSTR src, LPCSTR dst_dir, bool recursive, bool overwrite, SReport& rep)
{
	xr_string err;

	// the destination is the only side that has to be inside the project
	string_path dst_root;
	if (!ResolveInProject(dst_dir, dst_root, err))
	{
		Fail(rep, dst_dir, err);
		return;
	}

	string_path from;
	if (!ResolveSource(src, from, err))
	{
		Fail(rep, src, err);
		return;
	}

	if (!DirExists(from) && !FileExists(from))
	{
		// not on disk - it may still be an entry inside the game's archives
		if (!CopyFromGameArchive(src, dst_root, overwrite, rep))
			Fail(rep, src, xr_string("source not found"));
		return;
	}

	char target[MAX_PATH];
	if (!Join(target, sizeof(target), dst_root, LeafName(from)))
	{
		Fail(rep, src, xr_string("path too long"));
		return;
	}

	if (DirExists(from))
	{
		// copying a folder into itself or into its own subtree would feed the
		// walk with what it is writing
		if (IsInside(from, target))
		{
			Fail(rep, src, xr_string("destination is inside the source folder"));
			return;
		}
		if (!recursive)
		{
			Skip(rep, from, "folder (pass recursive=1)");
			return;
		}
		CopyTree(from, target, true, overwrite, rep);
		return;
	}

	if (!EnsureDir(dst_root))
	{
		Fail(rep, dst_root, Win32Reason("cannot create the target folder"));
		return;
	}
	if (0 == _stricmp(from, target))
	{
		Skip(rep, from, "source and destination are the same file");
		return;
	}
	CopyOneFile(from, target, overwrite, rep);
}

//------------------------------------------------------------------------------
// move
//------------------------------------------------------------------------------
void EditorFileOps::Move(LPCSTR src, LPCSTR dst_dir, bool recursive, bool overwrite, SReport& rep)
{
	xr_string err;

	string_path dst_root;
	if (!ResolveInProject(dst_dir, dst_root, err))
	{
		Fail(rep, dst_dir, err);
		return;
	}

	// a move deletes its source, so the source has to be writable too - i.e.
	// inside the project. DARF / Editor Content are copy-out sources only.
	string_path from;
	if (!ResolveInProject(src, from, err))
	{
		Fail(rep, src, err + " (a move deletes its source, so read-only sources can only be copied)");
		return;
	}

	if (!DirExists(from) && !FileExists(from))
	{
		Fail(rep, src, xr_string("source not found"));
		return;
	}

	char target[MAX_PATH];
	if (!Join(target, sizeof(target), dst_root, LeafName(from)))
	{
		Fail(rep, src, xr_string("path too long"));
		return;
	}
	if (0 == _stricmp(from, target))
	{
		Skip(rep, from, "source and destination are the same");
		return;
	}

	if (FileExists(from))
	{
		if (FileExists(target) && !overwrite)
		{
			Skip(rep, target, "already exists (pass overwrite=1)");
			return;
		}
		if (!EnsureDir(dst_root))
		{
			Fail(rep, dst_root, Win32Reason("cannot create the target folder"));
			return;
		}
		if (overwrite && FileExists(target))
			::SetFileAttributesA(target, FILE_ATTRIBUTE_NORMAL);

		DWORD flags = MOVEFILE_COPY_ALLOWED;					// cross-volume is legal
		if (overwrite) flags |= MOVEFILE_REPLACE_EXISTING;
		if (::MoveFileExA(from, target, flags))	++rep.files;
		else									Fail(rep, from, Win32Reason("move failed"));
		return;
	}

	// ---- folder ----
	if (IsInside(from, target))
	{
		Fail(rep, src, xr_string("destination is inside the source folder"));
		return;
	}
	if (!recursive)
	{
		Skip(rep, from, "folder (pass recursive=1)");
		return;
	}
	if (!EnsureDir(dst_root))
	{
		Fail(rep, dst_root, Win32Reason("cannot create the target folder"));
		return;
	}

	// same volume and a free name: a plain rename, counted by a cheap pre-walk
	if (!DirExists(target) && !FileExists(target))
	{
		int files = 0, dirs = 0;
		CountTree(from, files, dirs);
		if (::MoveFileExA(from, target, 0))
		{
			rep.files	+= files;
			rep.dirs	+= dirs;
			return;
		}
	}

	// merging into an existing folder, or a cross-volume move: copy the tree and
	// drop the source only if every single entry made it. A SKIP counts against
	// that just as much as a failure - deleting a source whose copy was skipped
	// (existing target, !overwrite) would destroy the only copy of those files.
	const int failed_before		= rep.failed;
	const int skipped_before	= rep.skipped;
	CopyTree(from, target, true, overwrite, rep);
	if (rep.failed != failed_before || rep.skipped != skipped_before)
	{
		Fail(rep, from, xr_string("source kept: part of the tree was not copied"));
		return;
	}
	// the copy already counted these files; the delete must not count them twice
	SReport cleanup;
	DeleteTree(from, cleanup);
	rep.failed += cleanup.failed;
	for (u32 i = 0; i < cleanup.failures.size(); ++i)
		Note(rep.failures, cleanup.failures[i].path.c_str(), cleanup.failures[i].reason);
}

//------------------------------------------------------------------------------
// delete
//------------------------------------------------------------------------------
void EditorFileOps::Delete(LPCSTR path, bool recursive, SReport& rep)
{
	xr_string err;

	string_path victim;
	if (!ResolveInProject(path, victim, err))
	{
		Fail(rep, path, err);
		return;
	}

	string_path root;
	if (LexicalNormalize(EditorProject::Root(), root, sizeof(root)) && 0 == _stricmp(root, victim))
	{
		Fail(rep, path, xr_string("refusing to delete the project root itself"));
		return;
	}

	if (FileExists(victim))
	{
		::SetFileAttributesA(victim, FILE_ATTRIBUTE_NORMAL);
		if (::DeleteFileA(victim))	++rep.files;
		else						Fail(rep, victim, Win32Reason("delete failed"));
		return;
	}
	if (!DirExists(victim))
	{
		Fail(rep, path, xr_string("not found"));
		return;
	}
	if (!recursive)
	{
		// RemoveDirectory only succeeds on an empty folder, which is exactly the
		// non-recursive contract
		if (::RemoveDirectoryA(victim))	++rep.dirs;
		else							Fail(rep, victim, xr_string("folder is not empty (pass recursive=1)"));
		return;
	}
	DeleteTree(victim, rep);
}

//------------------------------------------------------------------------------
// MCP
//------------------------------------------------------------------------------
static bool ArgBool(LPCSTR raw, LPCSTR field, bool def)
{
	char pat[64];
	sprintf_s(pat, "\"%s\"", field);
	const char* k = raw ? strstr(raw, pat) : 0;
	if (!k) return def;
	const char* c = strchr(k + xr_strlen(pat), ':');
	if (!c) return def;
	while (*++c == ' ' || *c == '"') {}
	// true / false plus the 1 / 0 spelling the commands document
	return 0 == strncmp(c, "true", 4) || *c == '1';
}

static void AppendDetails(xr_string& out, LPCSTR key, const xr_vector<EditorFileOps::SFailure>& v)
{
	out += ",\"";
	out += key;
	out += "\":[";
	for (u32 i = 0; i < v.size(); ++i)
	{
		if (i) out += ",";
		out += "{\"path\":\"";
		EditorGameContent::JsonAppendPath(out, v[i].path.c_str());
		out += "\",\"reason\":\"";
		EditorGameContent::JsonAppendPath(out, v[i].reason.c_str());
		out += "\"}";
	}
	out += "]";
}

static void AppendReport(xr_string& out, const EditorFileOps::SReport& rep)
{
	char tmp[192];
	sprintf_s(tmp, "{\"ok\":%s,\"files\":%d,\"folders\":%d,\"skipped\":%d,\"failed\":%d",
		rep.ok() ? "true" : "false", rep.files, rep.dirs, rep.skipped, rep.failed);
	out = tmp;
	AppendDetails(out, "errors", rep.failures);
	AppendDetails(out, "skips", rep.skips);
	// the detail lists are capped, so say when they no longer tell the whole story
	if (rep.failures.size() < u32(rep.failed) || rep.skips.size() < u32(rep.skipped))
		out += ",\"details_truncated\":true";
	out += "}";
}

// runs `fn` over a ';'-separated list of entries, accumulating one report
typedef void (*TEntryOp)(LPCSTR entry, LPCSTR dst, bool recursive, bool overwrite,
						 EditorFileOps::SReport& rep);

static void ForEachEntry(char* list, LPCSTR dst, bool recursive, bool overwrite,
						 EditorFileOps::SReport& rep, TEntryOp fn)
{
	for (char* cursor = list; cursor && *cursor;)
	{
		char* sep = strchr(cursor, ';');
		if (sep) *sep = 0;
		while (*cursor == ' ') ++cursor;
		for (char* e = cursor + xr_strlen(cursor); e > cursor && e[-1] == ' ';) *--e = 0;
		if (*cursor) fn(cursor, dst, recursive, overwrite, rep);
		cursor = sep ? sep + 1 : 0;
	}
}

static void OpCopy(LPCSTR entry, LPCSTR dst, bool recursive, bool overwrite, EditorFileOps::SReport& rep)
{
	EditorFileOps::Copy(entry, dst, recursive, overwrite, rep);
}

static void OpMove(LPCSTR entry, LPCSTR dst, bool recursive, bool overwrite, EditorFileOps::SReport& rep)
{
	EditorFileOps::Move(entry, dst, recursive, overwrite, rep);
}

static void OpDelete(LPCSTR entry, LPCSTR, bool recursive, bool, EditorFileOps::SReport& rep)
{
	EditorFileOps::Delete(entry, recursive, rep);
}

static void McpTransfer(LPCSTR raw, xr_string& out, TEntryOp fn)
{
	if (!EditorProject::Active())
	{
		out = "{\"ok\":false,\"error\":\"no project is open\"}";
		return;
	}

	char src[4096] = {}, dst[MAX_PATH] = {};
	if (!XFinedMCP::GetArg(raw, "src", src, sizeof(src)) || !src[0])
	{
		out = "{\"ok\":false,\"error\":\"missing 'src' argument (';'-separated list allowed)\"}";
		return;
	}
	if (!XFinedMCP::GetArg(raw, "dst", dst, sizeof(dst)) || !dst[0])
	{
		out = "{\"ok\":false,\"error\":\"missing 'dst' argument (target folder inside the project)\"}";
		return;
	}

	EditorFileOps::SReport rep;
	ForEachEntry(src, dst, ArgBool(raw, "recursive", true), ArgBool(raw, "overwrite", false), rep, fn);
	AppendReport(out, rep);
}

void EditorFileOps::McpCopy(LPCSTR raw, xr_string& out) { McpTransfer(raw, out, &OpCopy); }
void EditorFileOps::McpMove(LPCSTR raw, xr_string& out) { McpTransfer(raw, out, &OpMove); }

void EditorFileOps::McpDelete(LPCSTR raw, xr_string& out)
{
	if (!EditorProject::Active())
	{
		out = "{\"ok\":false,\"error\":\"no project is open\"}";
		return;
	}

	char path[4096] = {};
	if (!XFinedMCP::GetArg(raw, "path", path, sizeof(path)) || !path[0])
	{
		out = "{\"ok\":false,\"error\":\"missing 'path' argument (';'-separated list allowed)\"}";
		return;
	}

	SReport rep;
	ForEachEntry(path, "", ArgBool(raw, "recursive", false), false, rep, &OpDelete);
	AppendReport(out, rep);
}
