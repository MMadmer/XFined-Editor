#include "stdafx.h"
#include "EditorGameContent.h"
#include "EditorGameModes.h"
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

// Everything the game reads as text is cp1251; ImGui hands us UTF-8. Only
// captions travel this way - ids and keys are ascii by construction. A caption
// that is not valid UTF-8 was written by hand in cp1251 already and is passed
// through: converting it would turn every Cyrillic letter into '?'.
static xr_string Utf8ToCp1251(const char* src)
{
	xr_string out;
	if (!src || !src[0]) return out;
	const int wide_len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
	if (wide_len <= 0) { out = src; return out; }
	xr_vector<wchar_t> wide(wide_len);
	::MultiByteToWideChar(CP_UTF8, 0, src, -1, wide.data(), wide_len);
	const int ansi_len = ::WideCharToMultiByte(1251, 0, wide.data(), -1, NULL, 0, NULL, NULL);
	if (ansi_len <= 0) { out = src; return out; }
	xr_vector<char> ansi(ansi_len);
	::WideCharToMultiByte(1251, 0, wide.data(), -1, ansi.data(), ansi_len, "?", NULL);
	out = ansi.data();
	return out;
}

static xr_string XmlEscape(const char* src)
{
	xr_string out;
	for (const char* c = src; c && *c; ++c)
	{
		switch (*c)
		{
		case '&':	out += "&amp;";		break;
		case '<':	out += "&lt;";		break;
		case '>':	out += "&gt;";		break;
		default:	out += *c;			break;
		}
	}
	return out;
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
			else if (key == "target")	m.target = val;
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
	// editor-side bookkeeping: the engine ignores it, the export insists on it
	if (!m.target.empty())
		AppendKey(t, "target", m.target.c_str());
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

	// Only the manifest: it is what makes the folder a module at all. NO
	// folders - a module has whatever layout its author gives it ([vfs]
	// publishes any of it), and an empty project stays empty. The folders the
	// engine's fixed channels read (patch\, spawn\, scripts\, levels\,
	// gamedata\) appear when something is put in them, not before.
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

// The one place a mod must NEVER land: inside the game's own gamedata. The
// whole point of the module system is that a mod lives in its own folder and
// nothing ever merges into the game tree - so a target that would put files
// there is refused, not warned about.
static bool InsideGameGamedata(LPCSTR path)
{
	LPCSTR game = EditorProject::GameRoot();
	if (!game || !game[0]) return false;
	char guard[MAX_PATH];
	sprintf_s(guard, "%s\\gamedata", game);
	const size_t n = xr_strlen(guard);
	return 0 == _strnicmp(path, guard, n) && (path[n] == 0 || path[n] == '\\');
}

// A module that brings its OWN game mode has to put the checkbox on the
// new-game screen itself - the engine cannot, that screen is built in Lua by
// the game. Three generated files do it, one set per module however many modes
// it declares:
//
//   patch\xms_modes.xmlp            the checkboxes + labels, appended to the
//                                   screen's main_dialog
//   gamedata\configs\text\rus\<module id>_modes.xml   the captions
//   scripts\mode_register.script    creates the controls at runtime and writes
//                                   the [character_creation] keys the engine
//                                   turns back into active XMS modes
//
// The names are fixed, not derived from the mode: renaming a mode rewrites the
// same three files instead of leaving a stale checkbox behind in the project.
// The string table is the one file that lands in the shared game namespace, so
// that one carries the module id - two modules must not fight over it.
//
// mode_register is the name the engine reserves for generated startup code;
// scripts\register.script stays the author's own and is never touched here.
//
// Written only when the manifest declares a mode; a module targeting an
// existing mode needs none of it.
void EditorMod::GetOwnedFiles(LPCSTR project_root, xr_vector<xr_string>& rel)
{
	rel.push_back("mod.ltx");
	rel.push_back("patch\\xms_modes.xmlp");
	rel.push_back("scripts\\mode_register.script");
	// the string table carries the module id (it lands in the game's shared
	// namespace) - resolve it from the manifest on disk
	SManifest m;
	if (project_root && Load(project_root, m) && !m.id.empty())
		rel.push_back(xr_string("gamedata\\configs\\text\\rus\\") + m.id + "_modes.xml");
}

static bool WriteModeRegistration(LPCSTR project_root, const EditorMod::SManifest& m, xr_string& err)
{
	// A declaration that is GONE has to take its generated files with it:
	// they are hidden from the browser, so nothing else would ever remove
	// them, and a stale invisible xmlp still puts a checkbox in the game.
	if (m.provides_modes.empty())
	{
		string_path stale;
		sprintf_s(stale, "%s\\patch\\xms_modes.xmlp", project_root);
		if (::DeleteFileA(stale)) Msg("* [XMS] mode declaration gone - removed %s", stale);
		sprintf_s(stale, "%s\\scripts\\mode_register.script", project_root);
		if (::DeleteFileA(stale)) Msg("* [XMS] mode declaration gone - removed %s", stale);
		if (!m.id.empty())
		{
			sprintf_s(stale, "%s\\gamedata\\configs\\text\\rus\\%s_modes.xml", project_root, m.id.c_str());
			if (::DeleteFileA(stale)) Msg("* [XMS] mode declaration gone - removed %s", stale);
		}
		return true;
	}

	// Never trust the manifest here: it may have been hand-written or made by
	// an older build, and these ids become file names, XML node names and Lua
	// identifiers. An id like "..\..\x" or one with a quote in it would write
	// outside the project or emit a file the game cannot parse.
	xr_vector<EditorMod::SProvidesMode> modes;
	for (u32 i = 0; i < m.provides_modes.size(); ++i)
	{
		const xr_string& id = m.provides_modes[i].id;
		if (id.empty()) continue;
		if (!EditorMod::ValidateModeId(id.c_str()) || id.size() > 64)
		{
			err  = "invalid declared mode id '" + id + "' (allowed: a-z 0-9 _ . -, up to 64 chars)";
			return false;
		}
		modes.push_back(m.provides_modes[i]);
	}
	if (modes.empty()) return true;

	string_path path;
	xr_string body;
	body.resize(8192);

	// ---- xml: one control pair per mode, below the campaign column ----------
	// The rows go under the last one already there - the game's own, plus any
	// another module added - and the frame around the column grows to hold
	// them, pushing whatever sat below it down. Only layouts the linked game
	// actually ships are patched; a patch aimed at a missing file is just a
	// warning in the player's log.
	static const char* kTargets[] = { "ui_mm_faction_select_16.xml", "ui_mm_faction_select.xml" };
	xr_string xmlp = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n";
	int targets = 0;
	for (int t = 0; t < 2; ++t)
	{
		string_path rel;
		sprintf_s(rel, "configs\\ui\\%s", kTargets[t]);
		EditorGameModes::SSlot slot;
		xr_string slot_err;
		if (!EditorGameModes::SuggestSlot(rel, (int)modes.size(), m.id.c_str(), slot, slot_err))
			continue;
		++targets;

		sprintf_s(&body[0], body.size(), "<xms-patch target=\"ui\\%s\">\r\n", kTargets[t]);
		xmlp += body.c_str();
		if (!slot.frame_node.empty())
		{
			sprintf_s(&body[0], body.size(),
				"\t<set-attr node=\"main_dialog:%s\" name=\"height\" value=\"%d\"/>\r\n",
				slot.frame_node.c_str(), slot.frame_height);
			xmlp += body.c_str();
		}
		for (u32 s = 0; s < slot.shift.size(); ++s)
		{
			sprintf_s(&body[0], body.size(),
				"\t<set-attr node=\"main_dialog:%s\" name=\"y\" value=\"%d\"/>\r\n",
				slot.shift[s].node.c_str(), slot.shift[s].y);
			xmlp += body.c_str();
		}
		xmlp += "\t<append into=\"main_dialog\">\r\n";
		for (u32 i = 0; i < modes.size(); ++i)
		{
			const xr_string node	= "check_" + modes[i].id + "_mode";
			const xr_string string_id = "st_cap_" + node;
			const int dy = int(i) * slot.step;
			sprintf_s(&body[0], body.size(),
				"\t\t<cap_%s x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" stretch=\"1\">\r\n"
				"\t\t\t<text r=\"170\" g=\"170\" b=\"170\" font=\"letterica16\" align=\"l\" vert_align=\"c\">%s</text>\r\n"
				"\t\t</cap_%s>\r\n"
				"\t\t<%s x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" stretch=\"1\">\r\n"
				"\t\t</%s>\r\n",
				node.c_str(), slot.cap_x, slot.cap_y + dy, slot.cap_w, slot.cap_h,
				string_id.c_str(), node.c_str(),
				node.c_str(), slot.check_x, slot.check_y + dy, slot.check_w, slot.check_h,
				node.c_str());
			xmlp += body.c_str();
		}
		xmlp += "\t</append>\r\n</xms-patch>\r\n";
	}
	if (!targets)
	{
		err = "the linked game has no new-game screen layout - nowhere to put the mode checkbox";
		return false;
	}
	sprintf_s(path, "%s\\patch", project_root);
	CreateDirChain(path);
	sprintf_s(path, "%s\\patch\\xms_modes.xmlp", project_root);
	if (!WriteTextFile(path, xmlp.c_str())) { err = "cannot write the mode xml patch"; return false; }

	// ---- text: what the player reads ----------------------------------------
	// String tables are cp1251 like the rest of the game; the manifest caption
	// comes out of ImGui as UTF-8, so it is converted on the way out.
	xr_string strings = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n<string_table>\r\n";
	for (u32 i = 0; i < modes.size(); ++i)
	{
		const xr_string& title	= modes[i].title.empty() ? modes[i].id : modes[i].title;
		const xr_string caption	= XmlEscape(Utf8ToCp1251(title.c_str()).c_str());
		sprintf_s(&body[0], body.size(),
			"\t<string id=\"st_cap_check_%s_mode\">\r\n"
			"\t\t<text>%s</text>\r\n"
			"\t</string>\r\n",
			modes[i].id.c_str(), caption.c_str());
		strings += body.c_str();
	}
	strings += "</string_table>\r\n";
	sprintf_s(path, "%s\\gamedata\\configs\\text\\rus", project_root);
	CreateDirChain(path);
	sprintf_s(path, "%s\\gamedata\\configs\\text\\rus\\%s_modes.xml", project_root, m.id.c_str());
	if (!WriteTextFile(path, strings.c_str())) { err = "cannot write the mode caption"; return false; }

	// ---- script: create the control and store the choice --------------------
	// Runs from the engine's registrar pass (xms.load_registrars). faction_ui is
	// a luabind class, so its methods are wrapped by assignment - xms.hook only
	// resolves plain _G tables. Everything is pcall-guarded: a game whose screen
	// was replaced by another mod must still reach the menu, just without the
	// checkbox, and the log says so.
	sprintf_s(&body[0], body.size(),
		"-- generated by XFined Editor for module '%s'\r\n"
		"-- Puts this module's game mode(s) on the new-game screen.\r\n"
		"-- Rewritten on every export - edit the manifest, not this file.\r\n"
		"-- (scripts\\register.script is yours; nothing here touches it.)\r\n"
		"local MODES = {\r\n",
		m.id.c_str());
	xr_string lua = body.c_str();
	for (u32 i = 0; i < modes.size(); ++i)
	{
		sprintf_s(&body[0], body.size(),
			"    { id = \"%s\", node = \"main_dialog:check_%s_mode\","
			" cap = \"main_dialog:cap_check_%s_mode\", key = \"new_game_%s_mode\" },\r\n",
			modes[i].id.c_str(), modes[i].id.c_str(), modes[i].id.c_str(), modes[i].id.c_str());
		lua += body.c_str();
	}
	lua +=
		"}\r\n"
		"\r\n"
		"local ok_screen, screen = pcall(function() return ui_mm_faction_select end)\r\n"
		"local cls = ok_screen and screen and screen.faction_ui\r\n"
		"if not cls then\r\n"
		"    xms.log(\"! xms modes: no new-game screen, they can only be set from the console\")\r\n"
		"    return\r\n"
		"end\r\n"
		"\r\n"
		"-- The controls, added after the stock ones. The screen keeps its\r\n"
		"-- CScriptXmlInit local, so this parses the same layout again - the engine\r\n"
		"-- picks the aspect variant, exactly as the base call does.\r\n"
		"local base_init = cls.InitControls\r\n"
		"if type(base_init) == \"function\" then\r\n"
		"    cls.InitControls = function(self, f)\r\n"
		"        base_init(self, f)\r\n"
		"        self.xms_checks = {}\r\n"
		"        local xml = CScriptXmlInit()\r\n"
		"        xml:ParseFile(\"ui_mm_faction_select.xml\")\r\n"
		"        for i = 1, #MODES do\r\n"
		"            local m = MODES[i]\r\n"
		"            local ok, err = pcall(function()\r\n"
		"                xml:InitStatic(m.cap, self.dialog)\r\n"
		"                self.xms_checks[m.id] = xml:InitCheck(m.node, self.dialog)\r\n"
		"                self:Register(self.xms_checks[m.id], m.node)\r\n"
		"            end)\r\n"
		"            if not ok then xms.log(\"! \" .. m.id .. \" checkbox: \" .. tostring(err)) end\r\n"
		"        end\r\n"
		"    end\r\n"
		"else\r\n"
		"    xms.log(\"! xms modes: InitControls missing, checkboxes skipped\")\r\n"
		"end\r\n"
		"\r\n"
		"-- The choice, written where the engine reads active modes from. Only the\r\n"
		"-- literal true counts as ticked, on this side and in the game's own\r\n"
		"-- is_*_mode helpers alike.\r\n"
		"local base_start = cls.OnStartGame\r\n"
		"if type(base_start) == \"function\" then\r\n"
		"    cls.OnStartGame = function(self, ...)\r\n"
		"        pcall(function()\r\n"
		"            local cfg = axr_main and axr_main.config\r\n"
		"            if not cfg then return end\r\n"
		"            for i = 1, #MODES do\r\n"
		"                local m = MODES[i]\r\n"
		"                local ck = self.xms_checks and self.xms_checks[m.id]\r\n"
		"                cfg:w_value(\"character_creation\", m.key, ck and ck:GetCheck() and true or nil)\r\n"
		"            end\r\n"
		"        end)\r\n"
		"        return base_start(self, ...)\r\n"
		"    end\r\n"
		"else\r\n"
		"    xms.log(\"! xms modes: OnStartGame missing, nothing will be stored\")\r\n"
		"end\r\n"
		"\r\n"
		"xms.log(\"game mode(s) registered on the new-game screen: \" .. #MODES)\r\n";

	sprintf_s(path, "%s\\scripts", project_root);
	CreateDirChain(path);
	sprintf_s(path, "%s\\scripts\\mode_register.script", project_root);
	if (!WriteTextFile(path, lua.c_str())) { err = "cannot write the mode registrar script"; return false; }

	Msg("* [XMS] %d game mode(s) registered for %d layout(s)", int(modes.size()), targets);
	return true;
}

// Empties a folder, keeping the folder itself. The module export starts from
// this: a build is CLEAN, so whatever the previous one left - a part the author
// has since deleted, a file they renamed, a whole folder that is no longer part
// of the project - is gone before the new one is written. Mirroring folder by
// folder could not do that: it only ever visited the parts the project still
// had, so anything the project stopped having stayed in the module forever.
static int WipeTree(LPCSTR dir)
{
	int removed = 0;
	WIN32_FIND_DATAA fd;
	string_path mask;
	sprintf_s(mask, "%s\\*", dir);
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (INVALID_HANDLE_VALUE == h) return 0;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		string_path p;
		sprintf_s(p, "%s\\%s", dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			removed += WipeTree(p);
			::RemoveDirectoryA(p);
		}
		else
		{
			// a read-only file (came off a CD, or the game marked it) still goes
			::SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
			if (::DeleteFileA(p)) ++removed;
		}
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
	return removed;
}

// How much of the target the clean build would throw away, without throwing it
// away - the export asks this before wiping a folder that may not be its own.
static int CountOrphansRec(LPCSTR dst, LPCSTR src)
{
	int n = 0;
	WIN32_FIND_DATAA fd;
	string_path mask;
	sprintf_s(mask, "%s\\*", dst);
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (INVALID_HANDLE_VALUE == h) return 0;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		string_path d, s;
		sprintf_s(d, "%s\\%s", dst, fd.cFileName);
		sprintf_s(s, "%s\\%s", src, fd.cFileName);
		const DWORD sa = ::GetFileAttributesA(s);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (INVALID_FILE_ATTRIBUTES == sa || !(sa & FILE_ATTRIBUTE_DIRECTORY))
				n += CountFilesRec(d, 0);
			else
				n += CountOrphansRec(d, s);
		}
		else if (INVALID_FILE_ATTRIBUTES == sa || (sa & FILE_ATTRIBUTE_DIRECTORY))
			++n;
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
	return n;
}

bool EditorMod::Export(LPCSTR project_root, LPCSTR target_root, bool flat,
					   int& files, xr_string& out_path, xr_string& err, bool confirmed)
{
	files = 0; out_path.clear(); err.clear();
	if (!project_root || !project_root[0])	{ err = "no active project"; return false; }

	SManifest m;
	if (!Load(project_root, m))				{ err = "mod.ltx not found in the project"; return false; }
	if (!ValidateId(m.id.c_str()))			{ err = "module id missing or invalid in mod.ltx (allowed: a-z 0-9 _ . -)"; return false; }

	// A finished module always states which game it is for. "Nothing chosen"
	// is not a third option - it is how content ends up applied in a campaign
	// it was never built for.
	if (m.mode.empty() && m.provides_modes.empty() && 0 != _stricmp(m.target.c_str(), "default"))
	{
		err = "no target game mode: open Mod > Edit Manifest and pick one "
			  "(the ordinary game, a mode of the linked game, or a new mode this module adds)";
		return false;
	}

	// A declared mode is worthless until the player can tick it, so the files
	// that put it on the new-game screen are generated as part of the export.
	if (!flat && !WriteModeRegistration(project_root, m, err))
		return false;

	char target[MAX_PATH] = {};
	NormalizePath(target_root ? target_root : "", target, sizeof(target));
	if (!target[0])							{ err = "target folder not set"; return false; }
	if (xr_strlen(target) + m.id.size() + 32 >= MAX_PATH)
											{ err = "target path too long"; return false; }

	// A module goes to <game>\modules\, NOT to mods\: that one is the folder
	// JSGME manages, and a module sitting there is a module JSGME offers to
	// copy over the game - the merge a module exists to avoid. A flat overlay
	// is the opposite case: that IS a JSGME mod, so it keeps its own name.
	string_path dst;
	if (flat)	sprintf_s(dst, "%s\\gamedata_%s", target, m.id.c_str());
	else		sprintf_s(dst, "%s\\modules\\%s", target, m.id.c_str());
	if (InsideGameGamedata(dst) || InsideGameGamedata(target))
	{
		err = "target is inside the game's gamedata - a mod must stay in its own folder "
			  "(export to the game ROOT: the module goes to <game>\\modules\\<id>)";
		return false;
	}
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
		if (!c_patch && !c_spawn && !c_scripts && !c_levels && m.mode.empty() && m.provides_modes.empty())
			rep += "fully vanilla-compatible\r\n";
		else
		{
			// a flat overlay has no way to add a checkbox to the new-game screen
			if (!m.provides_modes.empty())
			{
				rep += "declared mode: ";
				rep += m.provides_modes[0].id;
				rep += " - NOT registered: a flat overlay cannot add it to the "
					   "new-game screen, export as a module for that\r\n";
			}
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

		// What ships is "everything the author made", not a fixed list of five
		// folder names: a module may keep any layout it likes and publish it
		// through mod.ltx [vfs]. Only the editor's own scratch and the sources
		// the game cannot read (rawdata scenes, .object files) stay behind.
		xr_vector<xr_string> parts;
		{
			WIN32_FIND_DATAA fd;
			string_path mask;
			sprintf_s(mask, "%s\\*", project_root);
			HANDLE h = ::FindFirstFileA(mask, &fd);
			if (INVALID_HANDLE_VALUE != h)
			{
				do
				{
					if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
					if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))	continue;
					if (EditorProject::IsEditorOnlyEntry(fd.cFileName))		continue;
					if (EditorProject::IsSourceOnlyEntry(fd.cFileName))		continue;
					parts.push_back(xr_string(fd.cFileName));
				} while (::FindNextFileA(h, &fd));
				::FindClose(h);
			}
		}

		// The build is CLEAN: the target module folder is emptied first, so what
		// ends up there is exactly this build and nothing a previous one left.
		// That is destructive when the target is somebody else's module that
		// happens to share the id - so say so, and only wipe once told to.
		if (!confirmed && DirExists(dst))
		{
			int stale = 0;
			for (u32 i = 0; i < parts.size(); ++i)
			{
				sprintf_s(src, "%s\\%s", project_root, parts[i].c_str());
				sprintf_s(b, "%s\\%s", dst, parts[i].c_str());
				if (DirExists(b)) stale += CountOrphansRec(b, src);
			}
			if (stale)
			{
				err = "target already holds a module: ";
				char n[64]; sprintf_s(n, "%d file(s) there are not in this project", stale);
				err += n;
				out_path = dst;
				return false;	// caller re-runs with `confirmed` once the user agrees
			}
		}

		if (const int wiped = WipeTree(dst))
			Msg("* [XMS] clean build: %d stale file(s) removed from %s", wiped, dst);

		sprintf_s(a, "%s\\mod.ltx", project_root);
		sprintf_s(b, "%s\\mod.ltx", dst);
		if (::CopyFileA(a, b, FALSE)) ++files;

		for (u32 i = 0; i < parts.size(); ++i)
		{
			sprintf_s(src, "%s\\%s", project_root, parts[i].c_str());
			sprintf_s(b, "%s\\%s", dst, parts[i].c_str());
			if (!DirExists(src)) continue;
			// the patch readme is authoring help, not module content
			files += CopyTreeCount(src, b, (0 == _stricmp(parts[i].c_str(), "patch")) ? "_readme.txt" : 0);
			// a part that carried nothing leaves no folder behind either -
			// RemoveDirectory only succeeds on an empty one, which is the test
			::RemoveDirectoryA(b);
		}

		// loose files the author dropped in the root travel too
		{
			WIN32_FIND_DATAA fd;
			string_path mask;
			sprintf_s(mask, "%s\\*", project_root);
			HANDLE h = ::FindFirstFileA(mask, &fd);
			if (INVALID_HANDLE_VALUE != h)
			{
				do
				{
					if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)		continue;
					if (EditorProject::IsEditorOnlyEntry(fd.cFileName))		continue;
					if (0 == _stricmp(fd.cFileName, "mod.ltx"))				continue;	// already copied
					sprintf_s(src, "%s\\%s", project_root, fd.cFileName);
					sprintf_s(b, "%s\\%s", dst, fd.cFileName);
					if (::CopyFileA(src, b, FALSE)) ++files;
				} while (::FindNextFileA(h, &fd));
				::FindClose(h);
			}
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
static char					s_EdTarget[32]	= {};
static char					s_EdPModeId[128]	= {};
static char					s_EdPModeTitle[256]	= {};
static char					s_EdRequires[2048]	= {};
static char					s_EdAfter[1024]		= {};
static char					s_EdBefore[1024]	= {};
static char					s_EdConflicts[2048]	= {};
static char					s_Target[MAX_PATH]	= {};
// measured once per modal opening: reading the game's layout is not frame work
static bool					s_SlotChecked		= false;
static char					s_SlotText[256]		= {};

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
	strncpy_s(s_EdTarget, sizeof(s_EdTarget), s_Keep.target.c_str(), _TRUNCATE);
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
	s_SlotChecked = false;
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
			"Fix it via Mod > Edit Manifest... (allowed: a-z 0-9 _ . -)");
		s_WantMessage = true;
		return;
	}
	char ini[MAX_PATH];
	ProjectIni(ini, sizeof(ini));
	::GetPrivateProfileStringA("xms", "export_target", "", s_Target, sizeof(s_Target), ini);
	// first export of a linked project: the game root IS the deploy target -
	// the module lands in <game>\modules\<id>, which is where the game reads it
	if (!s_Target[0] && EditorProject::GameRoot()[0])
		strncpy_s(s_Target, sizeof(s_Target), EditorProject::GameRoot(), _TRUNCATE);
	if (flat)	s_WantFlat = true;
	else		s_WantExport = true;
}

void EditorMod::RequestExportModule()	{ PrepareExport(false); }
void EditorMod::RequestExportFlat()		{ PrepareExport(true); }

//------------------------------------------------------------------------------
// one-click build into the linked game
//
// Every question the export modal asks has one right answer once a game is
// linked: the target is that game, the layout is a module, and the id comes
// from the manifest. So this path asks nothing - except before deleting files
// it did not put there.
//------------------------------------------------------------------------------
static bool					s_WantBuild		= false;

bool EditorMod::CanBuildIntoGame()
{
	return EditorProject::Active() && EditorProject::GameLinked() && EditorProject::GameRoot()[0];
}

void EditorMod::BuildTargetText(char* dst, u32 size)
{
	dst[0] = 0;
	if (!CanBuildIntoGame()) return;
	SManifest m;
	if (!Load(EditorProject::Root(), m) || !ValidateId(m.id.c_str())) return;
	sprintf_s(dst, size, "%s\\modules\\%s", EditorProject::GameRoot(), m.id.c_str());
}

void EditorMod::RequestBuildIntoGame()
{
	s_WantBuild = CanBuildIntoGame();
	if (s_WantBuild) return;
	sprintf_s(s_Message,
		"No linked game.\n\n"
		"Link one first - the build writes the module into\n"
		"<game>\\modules\\<id>, which is where the game reads it from.");
	s_WantMessage = true;
}

// runs inside the ImGui frame (from DrawUI), like the export modal's Export
static void RunBuildIntoGame()
{
	if (!s_WantBuild) return;
	s_WantBuild = false;

	EditorMod::SManifest m;
	if (!EditorMod::Load(EditorProject::Root(), m) || !EditorMod::ValidateId(m.id.c_str()))
	{
		sprintf_s(s_Message,
			"Module id is missing or invalid.\n"
			"Fix it via Mod > Edit Manifest... (allowed: a-z 0-9 _ . -)");
		s_WantMessage = true;
		return;
	}

	LPCSTR target = EditorProject::GameRoot();
	int files = 0;
	xr_string path, err;
	bool ok = EditorMod::Export(EditorProject::Root(), target, false, files, path, err);
	if (!ok && 0 == strncmp(err.c_str(), "target already holds a module", 29))
	{
		if (mrYes == ELog.DlgMsg(mtConfirmation, mbYes | mbNo,
			"%s\n\n%s\n\nThose files will be DELETED. Continue?", err.c_str(), path.c_str()))
			ok = EditorMod::Export(EditorProject::Root(), target, false, files, path, err, true);
		else
		{
			sprintf_s(s_Message, "Build cancelled.");
			s_WantMessage = true;
			return;
		}
	}
	if (ok)
	{
		char ini[MAX_PATH];
		ProjectIni(ini, sizeof(ini));
		::WritePrivateProfileStringA("xms", "export_target", target, ini);
		sprintf_s(s_Message,
			"Build complete: %d file(s) written to\n%s\n\n"
			"The game picks it up on the next start.\n"
			"Toggle it from the console:  xms_enable %s  /  xms_disable %s",
			files, path.c_str(), m.id.c_str(), m.id.c_str());
	}
	else
		sprintf_s(s_Message, "Build failed: %s", err.c_str());
	s_WantMessage = true;
}

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

	// ---- what game mode is this module for ---------------------------------
	// A module is built for exactly one of three things: the ordinary game, a
	// campaign that already exists in THIS install, or a campaign it brings
	// itself. The list is scanned from the linked game (a global mod like
	// Revolution II replaces the stock one), and is shown by the names the
	// player sees - nobody should have to know that "Dead Air Metro" is
	// new_game_metro_mode.
	ImGui::TextUnformatted("This module is for:");
	{
		xr_string scan_err;
		EditorGameModes::Scan(scan_err);
		const xr_vector<EditorGameModes::SMode>& modes = EditorGameModes::All();

		// which of the three the manifest currently describes
		// s_EdTarget records the choice itself: "default" is a decision, an
		// empty manifest is not, and the export can tell them apart
		int kind = s_EdPModeId[0] ? 2 : (s_EdMode[0] ? 1 : (0 == _stricmp(s_EdTarget, "default") ? 0 : -1));
		if (ImGui::RadioButton("the ordinary game (no mode)", 0 == kind))
		{
			kind = 0; s_EdMode[0] = 0; s_EdPModeId[0] = 0; s_EdPModeTitle[0] = 0;
			strcpy_s(s_EdTarget, "default");
		}
		if (ImGui::RadioButton("a mode already in the linked game", 1 == kind))
		{
			kind = 1; s_EdPModeId[0] = 0; s_EdPModeTitle[0] = 0;
			strcpy_s(s_EdTarget, "existing");
			if (!s_EdMode[0] && !modes.empty())
				strncpy_s(s_EdMode, sizeof(s_EdMode), modes[0].id.c_str(), _TRUNCATE);
		}
		if (ImGui::RadioButton("a NEW mode this module adds", 2 == kind))
		{
			kind = 2; s_EdMode[0] = 0;
			strcpy_s(s_EdTarget, "new");
		}
		if (kind < 0)
			ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "not chosen yet - the module cannot be exported");

		if (1 == kind)
		{
			// caption of whatever is selected, so the combo reads like the game
			LPCSTR current = s_EdMode;
			for (u32 i = 0; i < modes.size(); ++i)
				if (0 == _stricmp(modes[i].id.c_str(), s_EdMode)) { current = modes[i].caption.c_str(); break; }

			ImGui::SetNextItemWidth(360);
			if (ImGui::BeginCombo("##target_mode", current))
			{
				for (u32 i = 0; i < modes.size(); ++i)
				{
					const bool sel = (0 == _stricmp(modes[i].id.c_str(), s_EdMode));
					if (ImGui::Selectable(modes[i].caption.c_str(), sel))
						strncpy_s(s_EdMode, sizeof(s_EdMode), modes[i].id.c_str(), _TRUNCATE);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s  (%s)", modes[i].id.c_str(),
							modes[i].source == "game" ? "in the game" : modes[i].source.c_str());
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			if (ImGui::Button("Rescan")) { EditorGameModes::Invalidate(); s_SlotChecked = false; }

			if (modes.empty())
				ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s",
					scan_err.empty() ? "the linked game declares no modes - only options"
									 : scan_err.c_str());
		}
		else if (2 == kind)
		{
			ImGui::InputText("mode id", s_EdPModeId, sizeof(s_EdPModeId));
			ImGui::InputText("mode title", s_EdPModeTitle, sizeof(s_EdPModeTitle));
			ImGui::TextDisabled("the id the module gates on, and the name the player will see");

			// A mode nobody can tick does not exist. Export generates the three
			// files that put it on the new-game screen; measuring where the row
			// lands takes reading the game's layout, so it is done once and
			// refreshed by Rescan rather than every frame.
			if (!s_SlotChecked)
			{
				s_SlotChecked = true;
				EditorGameModes::SSlot slot;
				xr_string slot_err;
				if (EditorGameModes::SuggestSlot("configs\\ui\\ui_mm_faction_select_16.xml", 1, s_EdId, slot, slot_err) ||
					EditorGameModes::SuggestSlot("configs\\ui\\ui_mm_faction_select.xml", 1, s_EdId, slot, slot_err))
					sprintf_s(s_SlotText, "export adds the checkbox at %d,%d of the new-game screen",
							  slot.check_x, slot.check_y);
				else
					sprintf_s(s_SlotText, "no new-game screen in the linked game: %s", slot_err.c_str());
			}
			ImGui::TextDisabled("%s", s_SlotText);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"Export writes three files into the project and keeps them in sync:\n"
					"  patch\\<id>_mode.xmlp                     the checkbox and its label\n"
					"  gamedata\\configs\\text\\rus\\<id>_mode.xml  the caption above\n"
					"  scripts\\mode_register.script             hooks the screen at startup\n"
					"Edit the manifest, not those files - they are rewritten every export.\n"
					"scripts\\register.script is yours and is never touched.");
		}

		// What the mode gate actually covers. Level work is gated - spawn ops,
		// collision, ai-map, visuals and the game graph all ask ModuleApplies -
		// but files, configs and scripts mount at engine start, before any mode
		// is picked, so they apply in every game. Saying so here beats a player
		// wondering why a weapon rebalance leaked into another campaign.
		if (kind > 0)
		{
			string_path probe;
			sprintf_s(probe, "%s\\gamedata", EditorProject::Root());
			const int n_gamedata = CountFilesRec(probe, 0);
			sprintf_s(probe, "%s\\patch", EditorProject::Root());
			const int n_patch = CountFilesRec(probe, "_readme.txt");
			if (n_gamedata || n_patch)
			{
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
					"note: %d file(s) and %d config patch(es) are NOT mode-gated",
					n_gamedata, n_patch);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"Level work (spawns, collision, ai-map, visuals, the game graph) applies only in\n"
						"the chosen mode. Files under gamedata\\ and patches under patch\\ mount when the\n"
						"engine starts, before a mode is picked, so they apply in every game.");
			}
		}
	}
	const bool mode_ok = EditorMod::ValidateModeId(s_EdMode);
	if (!mode_ok)
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "invalid mode id: a-z 0-9 _ . - only (or empty)");
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
		m.target = s_EdTarget;
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
		: "Target game folder; the module goes to <target>\\modules\\<id>:");
	ImGui::SetNextItemWidth(480);
	ImGui::InputText("##target", s_Target, sizeof(s_Target));
	if (EditorProject::GameRoot()[0])
	{
		if (ImGui::Button(flat ? "Use linked game root" : "Deploy to linked game"))
			strncpy_s(s_Target, sizeof(s_Target), EditorProject::GameRoot(), _TRUNCATE);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", EditorProject::GameRoot());
	}
	ImGui::Separator();

	ImGui::BeginDisabled(!s_Target[0]);
	if (ImGui::Button("Export", ImVec2(120, 0)))
	{
		int files = 0;
		xr_string path, err;
		bool ok = EditorMod::Export(EditorProject::Root(), s_Target, flat, files, path, err);
		// the refusal above is a question, not a failure: name what would be
		// deleted and let the user decide
		if (!ok && 0 == strncmp(err.c_str(), "target already holds a module", 29))
		{
			if (mrYes == ELog.DlgMsg(mtConfirmation, mbYes | mbNo,
				"%s\n\n%s\n\nThose files will be DELETED. Continue?", err.c_str(), path.c_str()))
				ok = EditorMod::Export(EditorProject::Root(), s_Target, flat, files, path, err, true);
			else
			{
				sprintf_s(s_Message, "Export cancelled.");
				s_WantMessage = true;
				want = false;
				ImGui::CloseCurrentPopup();
				ImGui::EndDisabled();	// balances the BeginDisabled above
				ImGui::EndPopup();
				return;
			}
		}
		if (ok)
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
	RunBuildIntoGame();
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
	// The targeting choice itself. "default" cannot be inferred - a module with
	// no mode set either targets the ordinary game on purpose or was never
	// decided, and only the caller knows which - so it is stated explicitly;
	// picking a mode states it implicitly.
	if (XFinedMCP::GetArg(raw, "target", v, sizeof(v)))
	{
		if (0 != _stricmp(v, "default") && 0 != _stricmp(v, "existing") && 0 != _stricmp(v, "new"))
			{ out = "{\"ok\":false,\"error\":\"invalid target (allowed: default, existing, new)\"}"; return; }
		m.target = v;
	}
	else if (!m.provides_modes.empty())	m.target = "new";
	else if (!m.mode.empty())			m.target = "existing";
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
	// no target = deploy to the linked game: <game>\modules\<id> is where the
	// game actually reads modules from, so it is the default, not an option
	if (!XFinedMCP::GetArg(raw, "target", target, sizeof(target)) || !target[0])
	{
		if (!EditorProject::GameRoot()[0])
			{ out = "{\"ok\":false,\"error\":\"no 'target' and no linked game (see game_link)\"}"; return; }
		strncpy_s(target, sizeof(target), EditorProject::GameRoot(), _TRUNCATE);
	}
	const bool flat = ArgBool(raw, "flat", false);

	int files = 0;
	xr_string path, err;
	// MCP is the programmatic path: the caller already decided, and it must
	// never stall on a question
	if (!Export(EditorProject::Root(), target, flat, files, path, err, true))
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
