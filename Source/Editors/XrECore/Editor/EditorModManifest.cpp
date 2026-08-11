#include "stdafx.h"
#include "EditorModManifest.h"
#include "EditorProject.h"
#include "XFinedMCP.h"

//------------------------------------------------------------------------------
// small file/string helpers (WinAPI only, like EditorProject)
//------------------------------------------------------------------------------
static bool DirExists(const char* p)
{
	DWORD a = ::GetFileAttributesA(p);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool FileExists(const char* p)
{
	DWORD a = ::GetFileAttributesA(p);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void CreateDirChain(const char* folder)
{
	string_path acc = {};
	for (const char* p = folder;; ++p)
	{
		if (*p == '\\' || *p == 0)
		{
			if (acc[0] && acc[xr_strlen(acc) - 1] != ':') ::CreateDirectoryA(acc, NULL);
			if (*p == 0) break;
		}
		acc[p - folder] = *p;
		acc[p - folder + 1] = 0;
	}
}

static bool ReadTextFile(const char* path, xr_string& out)
{
	out.clear();
	HANDLE h = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) return false;
	const DWORD size = ::GetFileSize(h, NULL);
	if (size && size != INVALID_FILE_SIZE)
	{
		out.resize(size);
		DWORD rd = 0;
		if (::ReadFile(h, &out[0], size, &rd, NULL))	out.resize(rd);
		else											out.clear();
	}
	::CloseHandle(h);
	return true;
}

static bool WriteTextFile(const char* path, const char* text)
{
	HANDLE h = ::CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD wr = 0;
	const DWORD len = (DWORD)xr_strlen(text);
	const bool ok = !!::WriteFile(h, text, len, &wr, NULL) && wr == len;
	::CloseHandle(h);
	return ok;
}

static void Trim(xr_string& s)
{
	const size_t b = s.find_first_not_of(" \t\r\n");
	const size_t e = s.find_last_not_of(" \t\r\n");
	s = (b == xr_string::npos) ? "" : s.substr(b, e - b + 1);
}

// splits on `sep`, trims entries, drops empties; '\r' counts as '\n'
static void SplitList(xr_vector<xr_string>& dst, LPCSTR src, char sep)
{
	dst.clear();
	xr_string cur;
	for (const char* p = src;; ++p)
	{
		if (*p == sep || *p == 0 || (sep == '\n' && *p == '\r'))
		{
			Trim(cur);
			if (!cur.empty()) dst.push_back(cur);
			cur.clear();
			if (*p == 0) break;
		}
		else cur += *p;
	}
}

static void JoinList(const xr_vector<xr_string>& src, char* dst, u32 dst_size, LPCSTR sep)
{
	xr_string all;
	for (u32 i = 0; i < src.size(); ++i)
	{
		if (i) all += sep;
		all += src[i];
	}
	strncpy_s(dst, dst_size, all.c_str(), _TRUNCATE);
}

// forward slashes in, JSON-escaped double backslashes in - one clean path out
static void NormalizePath(const char* src, char* dst, u32 dst_size)
{
	u32 o = 0;
	for (const char* p = src; *p && o + 1 < dst_size; ++p)
	{
		const char c = (*p == '/') ? '\\' : *p;
		// collapse doubled separators but keep a UNC "\\server" prefix
		if (c == '\\' && o > 1 && dst[o - 1] == '\\') continue;
		dst[o++] = c;
	}
	while (o && dst[o - 1] == '\\') --o;
	dst[o] = 0;
}

//------------------------------------------------------------------------------
// json helpers
//------------------------------------------------------------------------------
static void JsonAppend(xr_string& out, LPCSTR s)
{
	for (const char* p = s; *p; ++p)
	{
		if (*p == '"' || *p == '\\') out += '\\';
		out += *p;
	}
}

static void JsonAppendPath(xr_string& out, LPCSTR s)
{
	for (const char* p = s; *p; ++p) out += (*p == '\\') ? '/' : *p;
}

static void JsonAppendArray(xr_string& out, const xr_vector<xr_string>& v)
{
	out += "[";
	for (u32 i = 0; i < v.size(); ++i)
	{
		if (i) out += ",";
		out += "\"";
		JsonAppend(out, v[i].c_str());
		out += "\"";
	}
	out += "]";
}

// bare (unquoted) bool argument: {"flat":true}
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

//------------------------------------------------------------------------------
// id rules
//------------------------------------------------------------------------------
bool EditorMod::ValidateId(LPCSTR id)
{
	if (!id || !id[0]) return false;
	for (const char* p = id; *p; ++p)
	{
		const char c = *p;
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
						c == '_' || c == '.' || c == '-';
		if (!ok) return false;
	}
	return true;
}

bool EditorMod::ValidateModeId(LPCSTR id)
{
	return !id || !id[0] || ValidateId(id);
}

void EditorMod::DeriveId(LPCSTR project_name, char* dst, u32 dst_size)
{
	u32 o = 0;
	for (const char* p = project_name ? project_name : ""; *p && o + 1 < dst_size; ++p)
	{
		char c = *p;
		if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
						c == '_' || c == '.' || c == '-';
		dst[o++] = ok ? c : '_';
	}
	dst[o] = 0;
	if (!o) strncpy_s(dst, dst_size, "unnamed_mod", _TRUNCATE);
}

//------------------------------------------------------------------------------
// mod.ltx parse / write
//------------------------------------------------------------------------------
bool EditorMod::Load(LPCSTR project_root, SManifest& m)
{
	m = SManifest();
	if (!project_root || !project_root[0]) return false;
	char path[MAX_PATH];
	sprintf_s(path, "%s\\mod.ltx", project_root);
	xr_string text;
	if (!ReadTextFile(path, text)) return false;

	enum ESec { secNone, secModule, secRequires, secOrder, secConflicts, secProvides, secExtra };
	ESec sec = secNone;

	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t nl = text.find('\n', pos);
		if (nl == xr_string::npos) nl = text.size();
		xr_string raw = text.substr(pos, nl - pos);
		pos = nl + 1;
		Trim(raw);

		// known sections drop comments; unknown ones are kept verbatim
		xr_string line = raw;
		const size_t sc = line.find(';');
		if (sc != xr_string::npos) { line = line.substr(0, sc); Trim(line); }

		if (!line.empty() && line[0] == '[')
		{
			const size_t close = line.find(']');
			xr_string sn = (close != xr_string::npos) ? line.substr(1, close - 1) : "";
			Trim(sn);
			for (size_t i = 0; i < sn.size(); ++i)
				if (sn[i] >= 'A' && sn[i] <= 'Z') sn[i] = char(sn[i] - 'A' + 'a');
			if      (sn == "module")		sec = secModule;
			else if (sn == "requires")		sec = secRequires;
			else if (sn == "order")			sec = secOrder;
			else if (sn == "conflicts")		sec = secConflicts;
			else if (sn == "provides_mode")	sec = secProvides;
			else { sec = secExtra; m.extra.push_back(raw); }
			continue;
		}
		if (sec == secExtra) { if (!raw.empty()) m.extra.push_back(raw); continue; }
		if (line.empty()) continue;

		if (sec == secModule || sec == secOrder || sec == secProvides)
		{
			xr_string key = line, val;
			const size_t eq = line.find('=');
			if (eq != xr_string::npos) { key = line.substr(0, eq); val = line.substr(eq + 1); }
			Trim(key); Trim(val);
			if (sec == secModule)
			{
				if      (key == "id")		m.id = val;
				else if (key == "name")		m.name = val;
				else if (key == "version")	m.version = val;
				else if (key == "api")		m.api = val;
				else if (key == "mode")		m.mode = val;
				else						m.module_extra.push_back(line);
			}
			else if (sec == secOrder)
			{
				if      (key == "after")	SplitList(m.after, val.c_str(), ',');
				else if (key == "before")	SplitList(m.before, val.c_str(), ',');
			}
			else
			{
				// each `id` line starts a new declared mode, `title` fills the last one
				if (key == "id")
				{
					SProvidesMode pm;
					pm.id = val;
					m.provides_modes.push_back(pm);
				}
				else if (key == "title" && !m.provides_modes.empty())
					m.provides_modes.back().title = val;
				else
					m.provides_extra.push_back(line);
			}
		}
		else if (sec == secRequires)	m.requires_list.push_back(line);
		else if (sec == secConflicts)	m.conflicts.push_back(line);
	}
	return true;
}

static void AppendKey(xr_string& out, LPCSTR key, LPCSTR val)
{
	char pad[16];
	sprintf_s(pad, "%-10s", key);	// keys are short literals only
	out += pad;
	out += " = ";
	out += val;
	out += "\r\n";
}

static void AppendJoined(xr_string& out, LPCSTR key, const xr_vector<xr_string>& v)
{
	xr_string all;
	for (u32 i = 0; i < v.size(); ++i)
	{
		if (i) all += ", ";
		all += v[i];
	}
	AppendKey(out, key, all.c_str());
}

bool EditorMod::Save(LPCSTR project_root, const SManifest& m)
{
	if (!project_root || !project_root[0]) return false;
	xr_string t;
	t += "[module]\r\n";
	AppendKey(t, "id", m.id.c_str());
	AppendKey(t, "name", m.name.c_str());
	AppendKey(t, "version", m.version.empty() ? "1.0.0" : m.version.c_str());
	AppendKey(t, "api", m.api.empty() ? "1" : m.api.c_str());
	// empty mode = active in all game modes; the key is omitted then
	if (!m.mode.empty())
		AppendKey(t, "mode", m.mode.c_str());
	for (u32 i = 0; i < m.module_extra.size(); ++i)
		{ t += m.module_extra[i]; t += "\r\n"; }
	if (!m.requires_list.empty())
	{
		t += "\r\n[requires]\r\n";
		for (u32 i = 0; i < m.requires_list.size(); ++i)
			{ t += m.requires_list[i]; t += "\r\n"; }
	}
	if (!m.after.empty() || !m.before.empty())
	{
		t += "\r\n[order]\r\n";
		if (!m.after.empty())	AppendJoined(t, "after", m.after);
		if (!m.before.empty())	AppendJoined(t, "before", m.before);
	}
	if (!m.conflicts.empty())
	{
		t += "\r\n[conflicts]\r\n";
		for (u32 i = 0; i < m.conflicts.size(); ++i)
			{ t += m.conflicts[i]; t += "\r\n"; }
	}
	if (!m.provides_modes.empty() || !m.provides_extra.empty())
	{
		t += "\r\n[provides_mode]\r\n";
		for (u32 i = 0; i < m.provides_modes.size(); ++i)
		{
			AppendKey(t, "id", m.provides_modes[i].id.c_str());
			if (!m.provides_modes[i].title.empty())
				AppendKey(t, "title", m.provides_modes[i].title.c_str());
		}
		for (u32 i = 0; i < m.provides_extra.size(); ++i)
			{ t += m.provides_extra[i]; t += "\r\n"; }
	}
	if (!m.extra.empty())
	{
		t += "\r\n";
		for (u32 i = 0; i < m.extra.size(); ++i)
			{ t += m.extra[i]; t += "\r\n"; }
	}
	char path[MAX_PATH];
	sprintf_s(path, "%s\\mod.ltx", project_root);
	return WriteTextFile(path, t.c_str());
}

//------------------------------------------------------------------------------
// scaffold
//------------------------------------------------------------------------------
void EditorMod::EnsureScaffold(LPCSTR project_root, LPCSTR project_name)
{
	if (!project_root || !project_root[0]) return;

	static const char* dirs[] = { "gamedata", "patch", "spawn", "scripts" };
	for (int i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); ++i)
	{
		char p[MAX_PATH];
		sprintf_s(p, "%s\\%s", project_root, dirs[i]);
		::CreateDirectoryA(p, NULL);
	}

	char mod[MAX_PATH];
	sprintf_s(mod, "%s\\mod.ltx", project_root);
	if (!FileExists(mod))
	{
		SManifest m;
		char id[128];
		DeriveId(project_name, id, sizeof(id));
		m.id = id;
		m.name = project_name ? project_name : "";
		m.version = "1.0.0";
		m.api = "1";
		if (Save(project_root, m))
			Msg("* [XMS] mod.ltx scaffolded: %s", mod);
	}

	char readme[MAX_PATH];
	sprintf_s(readme, "%s\\patch\\_readme.txt", project_root);
	if (!FileExists(readme))
		WriteTextFile(readme,
			"XMS patch directory.\r\n"
			"Put config directive patches here as *.ltxp files (edit keys of named LTX sections).\r\n"
			"Put XML patches here as *.xmlp files (edit nodes of XML documents).\r\n"
			"Patches change named entities instead of replacing whole files, so modules stack.\r\n"
			"The module manifest lives in mod.ltx at the project root.\r\n");
}

//------------------------------------------------------------------------------
// export
//------------------------------------------------------------------------------
// recursive copy, returns files copied; `skip_file` name is excluded everywhere
static int CopyTreeCount(const char* src, const char* dst, const char* skip_file)
{
	::CreateDirectoryA(dst, NULL);
	int files = 0;
	string_path mask;
	sprintf_s(mask, "%s\\*", src);
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		string_path a, b;
		sprintf_s(a, "%s\\%s", src, fd.cFileName);
		sprintf_s(b, "%s\\%s", dst, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			files += CopyTreeCount(a, b, skip_file);
		else
		{
			if (skip_file && 0 == _stricmp(fd.cFileName, skip_file)) continue;
			if (::CopyFileA(a, b, FALSE)) ++files;
		}
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
	return files;
}

static int CountFilesRec(const char* dir, const char* skip_file)
{
	int files = 0;
	string_path mask;
	sprintf_s(mask, "%s\\*", dir);
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		string_path a;
		sprintf_s(a, "%s\\%s", dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			files += CountFilesRec(a, skip_file);
		else if (!skip_file || 0 != _stricmp(fd.cFileName, skip_file))
			++files;
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
	return files;
}

bool EditorMod::Export(LPCSTR project_root, LPCSTR target_root, bool flat,
					   int& files, xr_string& out_path, xr_string& err)
{
	files = 0; out_path.clear(); err.clear();
	if (!project_root || !project_root[0])	{ err = "no active project"; return false; }

	SManifest m;
	if (!Load(project_root, m))				{ err = "mod.ltx not found in the project"; return false; }
	if (!ValidateId(m.id.c_str()))			{ err = "module id missing or invalid in mod.ltx (allowed: a-z 0-9 _ . -)"; return false; }

	char target[MAX_PATH] = {};
	NormalizePath(target_root ? target_root : "", target, sizeof(target));
	if (!target[0])							{ err = "target folder not set"; return false; }
	if (xr_strlen(target) + m.id.size() + 32 >= MAX_PATH)
											{ err = "target path too long"; return false; }

	string_path dst;
	if (flat)	sprintf_s(dst, "%s\\gamedata_%s", target, m.id.c_str());
	else		sprintf_s(dst, "%s\\mods\\%s", target, m.id.c_str());
	CreateDirChain(dst);
	if (!DirExists(dst))					{ err = "can't create target folder"; return false; }

	string_path src;
	if (flat)
	{
		sprintf_s(src, "%s\\gamedata", project_root);
		if (DirExists(src)) files += CopyTreeCount(src, dst, 0);

		// XMS-only content is not representable as a plain gamedata overlay
		string_path d_patch, d_spawn, d_scripts, d_levels;
		sprintf_s(d_patch, "%s\\patch", project_root);
		sprintf_s(d_spawn, "%s\\spawn", project_root);
		sprintf_s(d_scripts, "%s\\scripts", project_root);
		sprintf_s(d_levels, "%s\\levels", project_root);
		const int c_patch	= CountFilesRec(d_patch, "_readme.txt");
		const int c_spawn	= CountFilesRec(d_spawn, 0);
		const int c_scripts	= CountFilesRec(d_scripts, 0);
		const int c_levels	= CountFilesRec(d_levels, 0);

		xr_string rep = "XMS flat export report - module '";
		rep += m.id;
		rep += "'\r\n\r\n";
		char line[128];
		if (!c_patch && !c_spawn && !c_scripts && !c_levels && m.mode.empty())
			rep += "fully vanilla-compatible\r\n";
		else
		{
			if (c_patch)	{ sprintf_s(line, "patch: %d file(s) - NOT exported (XMS-only)\r\n", c_patch); rep += line; }
			if (c_spawn)	{ sprintf_s(line, "spawn: %d file(s) - NOT exported (XMS-only)\r\n", c_spawn); rep += line; }
			if (c_scripts)	{ sprintf_s(line, "scripts: %d file(s) - NOT exported (XMS-only)\r\n", c_scripts); rep += line; }
			if (c_levels)	{ sprintf_s(line, "levels: %d overlay file(s) - NOT exported (XMS-only)\r\n", c_levels); rep += line; }
			if (!m.mode.empty())
			{
				rep += "target mode: ";
				rep += m.mode;
				rep += " - engine gating is XMS-only (NOT exported flat)\r\n";
			}
		}
		string_path rp;
		sprintf_s(rp, "%s\\_xms_export_report.txt", dst);
		WriteTextFile(rp, rep.c_str());
	}
	else
	{
		string_path a, b;
		sprintf_s(a, "%s\\mod.ltx", project_root);
		sprintf_s(b, "%s\\mod.ltx", dst);
		if (::CopyFileA(a, b, FALSE)) ++files;

		// levels\ carries the scene-baked overlays (collision/visuals)
		static const char* parts[] = { "gamedata", "patch", "spawn", "scripts", "levels" };
		for (int i = 0; i < (int)(sizeof(parts) / sizeof(parts[0])); ++i)
		{
			sprintf_s(src, "%s\\%s", project_root, parts[i]);
			if (!DirExists(src)) continue;
			sprintf_s(b, "%s\\%s", dst, parts[i]);
			// the patch readme is authoring help, not module content
			files += CopyTreeCount(src, b, (0 == strcmp(parts[i], "patch")) ? "_readme.txt" : 0);
		}
	}

	out_path = dst;
	Msg("* [XMS] export %s: %d file(s) -> %s", flat ? "flat" : "module", files, dst);
	return true;
}

//------------------------------------------------------------------------------
// UI: File\Mod modals
//------------------------------------------------------------------------------
static const char* kTitleManifest	= "XFined Editor - Mod Manifest";
static const char* kTitleExport		= "XFined Editor - Export XMS Module";
static const char* kTitleFlat		= "XFined Editor - Export Flat Gamedata";
static const char* kTitleMessage	= "XFined Editor - Mod Export Result";

static bool					s_WantManifest	= false;
static bool					s_WantExport	= false;
static bool					s_WantFlat		= false;
static bool					s_WantMessage	= false;
static char					s_Message[1024]	= {};

static EditorMod::SManifest	s_Keep;			// api/extra preserved across the edit
static char					s_EdId[128]		= {};
static char					s_EdName[256]	= {};
static char					s_EdVersion[64]	= {};
static char					s_EdMode[128]	= {};
static char					s_EdPModeId[128]	= {};
static char					s_EdPModeTitle[256]	= {};
static char					s_EdRequires[2048]	= {};
static char					s_EdAfter[1024]		= {};
static char					s_EdBefore[1024]	= {};
static char					s_EdConflicts[2048]	= {};
static char					s_Target[MAX_PATH]	= {};

static void ProjectIni(char* dst, u32 size)
{
	sprintf_s(dst, size, "%s\\project.ltx", EditorProject::Root());
}

void EditorMod::RequestEditManifest()
{
	if (!EditorProject::Active()) return;
	if (!Load(EditorProject::Root(), s_Keep))
	{
		EnsureScaffold(EditorProject::Root(), EditorProject::Name());
		Load(EditorProject::Root(), s_Keep);
	}
	strncpy_s(s_EdId, sizeof(s_EdId), s_Keep.id.c_str(), _TRUNCATE);
	strncpy_s(s_EdName, sizeof(s_EdName), s_Keep.name.c_str(), _TRUNCATE);
	strncpy_s(s_EdVersion, sizeof(s_EdVersion), s_Keep.version.c_str(), _TRUNCATE);
	strncpy_s(s_EdMode, sizeof(s_EdMode), s_Keep.mode.c_str(), _TRUNCATE);
	// the modal edits the first declared mode; extra pairs survive untouched
	s_EdPModeId[0] = 0; s_EdPModeTitle[0] = 0;
	if (!s_Keep.provides_modes.empty())
	{
		strncpy_s(s_EdPModeId, sizeof(s_EdPModeId), s_Keep.provides_modes[0].id.c_str(), _TRUNCATE);
		strncpy_s(s_EdPModeTitle, sizeof(s_EdPModeTitle), s_Keep.provides_modes[0].title.c_str(), _TRUNCATE);
	}
	JoinList(s_Keep.requires_list, s_EdRequires, sizeof(s_EdRequires), "\n");
	JoinList(s_Keep.after, s_EdAfter, sizeof(s_EdAfter), "\n");
	JoinList(s_Keep.before, s_EdBefore, sizeof(s_EdBefore), "\n");
	JoinList(s_Keep.conflicts, s_EdConflicts, sizeof(s_EdConflicts), "\n");
	s_WantManifest = true;
}

static void PrepareExport(bool flat)
{
	if (!EditorProject::Active()) return;
	EditorMod::SManifest m;
	if (!EditorMod::Load(EditorProject::Root(), m) || !EditorMod::ValidateId(m.id.c_str()))
	{
		sprintf_s(s_Message,
			"Module id is missing or invalid.\n"
			"Fix it via File > Mod > Edit Manifest... (allowed: a-z 0-9 _ . -)");
		s_WantMessage = true;
		return;
	}
	char ini[MAX_PATH];
	ProjectIni(ini, sizeof(ini));
	::GetPrivateProfileStringA("xms", "export_target", "", s_Target, sizeof(s_Target), ini);
	if (flat)	s_WantFlat = true;
	else		s_WantExport = true;
}

void EditorMod::RequestExportModule()	{ PrepareExport(false); }
void EditorMod::RequestExportFlat()		{ PrepareExport(true); }

static void DrawManifestModal()
{
	if (!s_WantManifest) return;
	ImGui::SetNextWindowSize(ImVec2(620, 720), ImGuiCond_FirstUseEver);
	if (!ImGui::BeginPopupModal(kTitleManifest, &s_WantManifest, 0, true))
		return;

	ImGui::InputText("id", s_EdId, sizeof(s_EdId));
	const bool id_ok = EditorMod::ValidateId(s_EdId);
	if (!id_ok)
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "invalid id: non-empty, a-z 0-9 _ . - only");
	ImGui::InputText("name", s_EdName, sizeof(s_EdName));
	ImGui::InputText("version", s_EdVersion, sizeof(s_EdVersion));
	ImGui::Separator();

	// game mode targeting (XMS: [module] mode) + declared mode ([provides_mode])
	ImGui::InputText("target mode", s_EdMode, sizeof(s_EdMode));
	ImGui::TextDisabled("game mode id this module is limited to; empty = all modes");
	const bool mode_ok = EditorMod::ValidateModeId(s_EdMode);
	if (!mode_ok)
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "invalid mode id: a-z 0-9 _ . - only (or empty)");
	ImGui::TextUnformatted("Provides new mode:");
	ImGui::InputText("mode id", s_EdPModeId, sizeof(s_EdPModeId));
	ImGui::InputText("mode title (string id)", s_EdPModeTitle, sizeof(s_EdPModeTitle));
	const bool pmode_ok = EditorMod::ValidateModeId(s_EdPModeId);
	if (!pmode_ok)
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "invalid mode id: a-z 0-9 _ . - only (or empty)");
	ImGui::Separator();
	ImGui::TextDisabled("one entry per line");
	ImGui::InputTextMultiline("requires", s_EdRequires, sizeof(s_EdRequires), ImVec2(-1, 64));
	ImGui::InputTextMultiline("after", s_EdAfter, sizeof(s_EdAfter), ImVec2(-1, 48));
	ImGui::InputTextMultiline("before", s_EdBefore, sizeof(s_EdBefore), ImVec2(-1, 48));
	ImGui::InputTextMultiline("conflicts", s_EdConflicts, sizeof(s_EdConflicts), ImVec2(-1, 64));
	ImGui::Separator();

	ImGui::BeginDisabled(!id_ok || !mode_ok || !pmode_ok);
	if (ImGui::Button("Save", ImVec2(120, 0)))
	{
		EditorMod::SManifest m = s_Keep;
		m.id = s_EdId;
		m.name = s_EdName;
		m.version = s_EdVersion;
		m.mode = s_EdMode;
		// only the first declared mode is edited here; empty id drops it
		if (s_EdPModeId[0])
		{
			if (m.provides_modes.empty()) m.provides_modes.push_back(EditorMod::SProvidesMode());
			m.provides_modes[0].id = s_EdPModeId;
			m.provides_modes[0].title = s_EdPModeTitle;
		}
		else if (!m.provides_modes.empty())
			m.provides_modes.erase(m.provides_modes.begin());
		SplitList(m.requires_list, s_EdRequires, '\n');
		SplitList(m.after, s_EdAfter, '\n');
		SplitList(m.before, s_EdBefore, '\n');
		SplitList(m.conflicts, s_EdConflicts, '\n');
		if (!EditorMod::Save(EditorProject::Root(), m))
			ELog.DlgMsg(mtError, "Can't write mod.ltx.");
		s_WantManifest = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0)))
	{
		s_WantManifest = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

static void DrawExportModal(bool flat)
{
	bool& want = flat ? s_WantFlat : s_WantExport;
	if (!want) return;
	ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
	if (!ImGui::BeginPopupModal(flat ? kTitleFlat : kTitleExport, &want, ImGuiWindowFlags_AlwaysAutoResize, true))
		return;

	ImGui::TextUnformatted(flat
		? "Target folder; a gamedata_<id> folder is created inside:"
		: "Target mods root folder; the module goes to <target>\\mods\\<id>:");
	ImGui::SetNextItemWidth(480);
	ImGui::InputText("##target", s_Target, sizeof(s_Target));
	ImGui::Separator();

	ImGui::BeginDisabled(!s_Target[0]);
	if (ImGui::Button("Export", ImVec2(120, 0)))
	{
		int files = 0;
		xr_string path, err;
		if (EditorMod::Export(EditorProject::Root(), s_Target, flat, files, path, err))
		{
			char ini[MAX_PATH];
			ProjectIni(ini, sizeof(ini));
			::WritePrivateProfileStringA("xms", "export_target", s_Target, ini);
			sprintf_s(s_Message, "Export complete: %d file(s) copied to\n%s", files, path.c_str());
		}
		else
			sprintf_s(s_Message, "Export failed: %s", err.c_str());
		s_WantMessage = true;
		want = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0)))
	{
		want = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

static void DrawMessageModal()
{
	if (!s_WantMessage) return;
	if (!ImGui::BeginPopupModal(kTitleMessage, &s_WantMessage, ImGuiWindowFlags_AlwaysAutoResize, true))
		return;
	ImGui::TextUnformatted(s_Message);
	ImGui::Separator();
	if (ImGui::Button("OK", ImVec2(120, 0)))
	{
		s_WantMessage = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void EditorMod::DrawUI()
{
	if (!EditorProject::Active()) return;
	DrawManifestModal();
	DrawExportModal(false);
	DrawExportModal(true);
	DrawMessageModal();
}

//------------------------------------------------------------------------------
// MCP commands
//------------------------------------------------------------------------------
void EditorMod::McpManifest(xr_string& out)
{
	if (!EditorProject::Active())
		{ out = "{\"ok\":false,\"error\":\"no active project\"}"; return; }
	SManifest m;
	if (!Load(EditorProject::Root(), m))
		{ out = "{\"ok\":false,\"error\":\"mod.ltx not found in the project\"}"; return; }

	out = "{\"ok\":true,\"id\":\"";
	JsonAppend(out, m.id.c_str());
	out += "\",\"name\":\"";
	JsonAppend(out, m.name.c_str());
	out += "\",\"version\":\"";
	JsonAppend(out, m.version.c_str());
	out += "\",\"mode\":\"";
	JsonAppend(out, m.mode.c_str());
	out += "\",\"provides_mode\":[";
	for (u32 i = 0; i < m.provides_modes.size(); ++i)
	{
		if (i) out += ",";
		out += "{\"id\":\"";
		JsonAppend(out, m.provides_modes[i].id.c_str());
		out += "\",\"title\":\"";
		JsonAppend(out, m.provides_modes[i].title.c_str());
		out += "\"}";
	}
	out += "],\"requires\":";
	JsonAppendArray(out, m.requires_list);
	out += ",\"after\":";
	JsonAppendArray(out, m.after);
	out += ",\"before\":";
	JsonAppendArray(out, m.before);
	out += ",\"conflicts\":";
	JsonAppendArray(out, m.conflicts);
	out += ",\"has_dirs\":{";
	static const char* dirs[] = { "gamedata", "patch", "spawn", "scripts", "levels" };
	for (int i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); ++i)
	{
		char p[MAX_PATH];
		sprintf_s(p, "%s\\%s", EditorProject::Root(), dirs[i]);
		char item[64];
		sprintf_s(item, "%s\"%s\":%s", i ? "," : "", dirs[i], DirExists(p) ? "true" : "false");
		out += item;
	}
	out += "}}";
}

void EditorMod::McpSetManifest(LPCSTR raw, xr_string& out)
{
	if (!EditorProject::Active())
		{ out = "{\"ok\":false,\"error\":\"no active project\"}"; return; }
	SManifest m;
	if (!Load(EditorProject::Root(), m))
	{
		// a pre-XMS project: scaffold the manifest, then edit it
		EnsureScaffold(EditorProject::Root(), EditorProject::Name());
		Load(EditorProject::Root(), m);
	}

	char v[2048];
	if (XFinedMCP::GetArg(raw, "id", v, sizeof(v)))
	{
		if (!ValidateId(v))
			{ out = "{\"ok\":false,\"error\":\"invalid id (allowed: a-z 0-9 _ . -)\"}"; return; }
		m.id = v;
	}
	if (XFinedMCP::GetArg(raw, "name", v, sizeof(v)))		m.name = v;
	if (XFinedMCP::GetArg(raw, "version", v, sizeof(v)))	m.version = v;
	if (XFinedMCP::GetArg(raw, "mode", v, sizeof(v)))
	{
		if (!ValidateModeId(v))
			{ out = "{\"ok\":false,\"error\":\"invalid mode id (allowed: a-z 0-9 _ . - or empty)\"}"; return; }
		m.mode = v;
	}
	// provides_mode edits touch only the first declared mode; empty id drops it
	if (XFinedMCP::GetArg(raw, "provides_mode_id", v, sizeof(v)))
	{
		if (!ValidateModeId(v))
			{ out = "{\"ok\":false,\"error\":\"invalid provides_mode_id (allowed: a-z 0-9 _ . - or empty)\"}"; return; }
		if (v[0])
		{
			if (m.provides_modes.empty()) m.provides_modes.push_back(SProvidesMode());
			m.provides_modes[0].id = v;
		}
		else if (!m.provides_modes.empty())
			m.provides_modes.erase(m.provides_modes.begin());
	}
	if (XFinedMCP::GetArg(raw, "provides_mode_title", v, sizeof(v)) && !m.provides_modes.empty())
		m.provides_modes[0].title = v;
	if (XFinedMCP::GetArg(raw, "requires", v, sizeof(v)))	SplitList(m.requires_list, v, ',');
	if (XFinedMCP::GetArg(raw, "after", v, sizeof(v)))		SplitList(m.after, v, ',');
	if (XFinedMCP::GetArg(raw, "before", v, sizeof(v)))		SplitList(m.before, v, ',');
	if (XFinedMCP::GetArg(raw, "conflicts", v, sizeof(v)))	SplitList(m.conflicts, v, ',');

	if (!ValidateId(m.id.c_str()))
		{ out = "{\"ok\":false,\"error\":\"module id missing or invalid (allowed: a-z 0-9 _ . -)\"}"; return; }
	if (!Save(EditorProject::Root(), m))
		{ out = "{\"ok\":false,\"error\":\"can't write mod.ltx\"}"; return; }
	McpManifest(out);	// respond with the fresh state
}

void EditorMod::McpExport(LPCSTR raw, xr_string& out)
{
	if (!EditorProject::Active())
		{ out = "{\"ok\":false,\"error\":\"no active project\"}"; return; }
	char target[MAX_PATH];
	if (!XFinedMCP::GetArg(raw, "target", target, sizeof(target)))
		{ out = "{\"ok\":false,\"error\":\"missing 'target' argument\"}"; return; }
	const bool flat = ArgBool(raw, "flat", false);

	int files = 0;
	xr_string path, err;
	if (!Export(EditorProject::Root(), target, flat, files, path, err))
	{
		out = "{\"ok\":false,\"error\":\"";
		JsonAppend(out, err.c_str());
		out += "\"}";
		return;
	}
	char head[64];
	sprintf_s(head, "{\"ok\":true,\"files\":%d,\"path\":\"", files);
	out = head;
	JsonAppendPath(out, path.c_str());
	out += "\"}";
}
