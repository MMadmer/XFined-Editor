#include "stdafx.h"
#include "EditorGameModes.h"
#include "EditorGameContent.h"
#include "EditorProject.h"
#include "XFinedMCP.h"

namespace
{
static xr_vector<EditorGameModes::SMode>	s_Modes;
static bool									s_Scanned	= false;
static xr_string							s_Error;

// The game's text and UI files are cp1251; ImGui draws UTF-8. Only the
// captions travel through here - ids and keys are ascii by construction.
xr_string Cp1251ToUtf8(LPCSTR src)
{
	xr_string out;
	if (!src) return out;
	const int wide_len = ::MultiByteToWideChar(1251, 0, src, -1, NULL, 0);
	if (wide_len <= 0) { out = src; return out; }
	xr_vector<wchar_t> wide(wide_len);
	::MultiByteToWideChar(1251, 0, src, -1, wide.data(), wide_len);
	const int utf_len = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, NULL, 0, NULL, NULL);
	if (utf_len <= 0) { out = src; return out; }
	xr_vector<char> utf(utf_len);
	::WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, utf.data(), utf_len, NULL, NULL);
	out = utf.data();
	return out;
}

// mod.ltx is written by this editor, so its captions are already UTF-8 - but a
// hand-written manifest may well be cp1251 like every other file in the game.
// Valid UTF-8 is left alone; anything else is read as cp1251.
xr_string CaptionFromManifest(LPCSTR src)
{
	if (!src) return xr_string();
	const int ok = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
	return ok > 0 ? xr_string(src) : Cp1251ToUtf8(src);
}

// whole item of the linked game as a string; empty when it is not there
xr_string ReadGameText(LPCSTR rel)
{
	xr_string text;
	const int idx = EditorGameContent::Find(rel);
	if (idx < 0) return text;
	u32 size = 0;
	u8* bytes = EditorGameContent::ReadBytes(idx, size);
	if (bytes && size)
	{
		text.assign((const char*)bytes, (const char*)bytes + size);
		text += '\0';
	}
	EditorGameContent::FreeBytes(bytes);
	return text;
}

int AttrInt(const xr_string& text, size_t tag_at, LPCSTR attr, int def)
{
	const size_t close = text.find('>', tag_at);
	if (close == xr_string::npos) return def;
	char pat[32];
	sprintf_s(pat, "%s=\"", attr);
	const size_t len = xr_strlen(pat);
	// the name has to START where it is found: "x=" also lives inside "max=",
	// and "height=" inside "item_height=", and both are real X-Ray attributes
	for (size_t at = text.find(pat, tag_at); at != xr_string::npos && at < close;
		 at = text.find(pat, at + len))
	{
		const char before = at ? text[at - 1] : ' ';
		if (before == ' ' || before == '\t' || before == '\r' || before == '\n')
			return atoi(text.c_str() + at + len);
	}
	return def;
}

// new_game_metro_mode -> metro, new_game_good_loot -> good_loot
xr_string NormalizeId(const xr_string& config_key)
{
	xr_string id = config_key;
	if (0 == _strnicmp(id.c_str(), "new_game_", 9))	id.erase(0, 9);
	const size_t n = id.size();
	if (n > 5 && 0 == _stricmp(id.c_str() + n - 5, "_mode"))	id.erase(n - 5);
	return id;
}

// Lua block comments hide whole InitCheck calls: Dead Air's story and azazel
// checkboxes sit inside --[[ ... --]] with the call itself unprefixed, so a
// per-line check alone would call them live.
bool InsideBlockComment(const xr_string& script, size_t at)
{
	bool block = false;
	for (size_t i = 0; i + 1 < at; ++i)
	{
		if (block)
		{
			if (script[i] == ']' && script[i + 1] == ']') { block = false; ++i; }
			continue;
		}
		if (script[i] != '-' || script[i + 1] != '-') continue;
		// --[[ opens a block; anything else is a line comment, skip to its end
		if (i + 3 < at && script[i + 2] == '[' && script[i + 3] == '[') { block = true; i += 3; }
		else
		{
			const size_t eol = script.find('\n', i);
			if (eol == xr_string::npos || eol >= at) return block;
			i = eol;
		}
	}
	return block;
}

// A checkbox is only real when the script creates it: Dead Air ships several
// that are commented out (azazel, story, and DAR2's km), and offering those
// would target a mode the player can never pick.
bool CheckboxLive(const xr_string& script, const xr_string& node)
{
	xr_string needle = "main_dialog:";
	needle += node;
	size_t at = 0;
	while ((at = script.find(needle, at)) != xr_string::npos)
	{
		// walk back to the line start and see whether it is commented out
		size_t bol = script.rfind('\n', at);
		bol = (bol == xr_string::npos) ? 0 : bol + 1;
		xr_string line = script.substr(bol, at - bol);
		size_t first = line.find_first_not_of(" \t");
		const bool commented = (first != xr_string::npos) && 0 == strncmp(line.c_str() + first, "--", 2);
		if (!commented && line.find("InitCheck") != xr_string::npos && !InsideBlockComment(script, at))
			return true;
		at += needle.size();
	}
	return false;
}

// st_cap_check_metro_mode -> "Dead Air Metro", from whatever language the
// install actually ships. Scans every text xml once, on demand.
void ResolveCaptions(xr_vector<EditorGameModes::SMode>& modes)
{
	// caption still holds the string id at this point; a parallel flag records
	// what has been resolved, because a resolved caption is arbitrary text and
	// cannot be told from an id by looking at it
	xr_vector<xr_string> string_ids(modes.size());
	bool any = false;
	for (u32 i = 0; i < modes.size(); ++i)
	{
		string_ids[i] = modes[i].caption;
		modes[i].caption.clear();
		if (!string_ids[i].empty()) any = true;
	}
	if (!any)
	{
		for (u32 i = 0; i < modes.size(); ++i) modes[i].caption = modes[i].id;
		return;
	}

	const int total = EditorGameContent::Count();
	for (int i = 0; i < total; ++i)
	{
		const EditorGameContent::SItem* it = EditorGameContent::Get(i);
		if (!it) continue;
		if (0 != _strnicmp(it->path.c_str(), "configs\\text\\", 13))	continue;
		if (it->path.size() < 4 || 0 != _stricmp(it->path.c_str() + it->path.size() - 4, ".xml")) continue;

		const xr_string text = ReadGameText(it->path.c_str());
		if (text.empty()) continue;

		for (u32 m = 0; m < modes.size(); ++m)
		{
			EditorGameModes::SMode& mode = modes[m];
			if (string_ids[m].empty() || !mode.caption.empty()) continue;	// nothing to look up, or done
			xr_string pat = "id=\"";
			pat += string_ids[m];
			pat += "\"";
			const size_t at = text.find(pat);
			if (at == xr_string::npos) continue;
			const size_t open = text.find("<text>", at);
			const size_t end  = (open == xr_string::npos) ? xr_string::npos : text.find("</text>", open);
			if (open == xr_string::npos || end == xr_string::npos) continue;
			xr_string raw = text.substr(open + 6, end - open - 6);
			mode.caption = Cp1251ToUtf8(raw.c_str());
		}
	}

	// a string id nobody defines stays readable rather than becoming noise
	for (u32 m = 0; m < modes.size(); ++m)
		if (modes[m].caption.empty())
			modes[m].caption = modes[m].id;
}

// ---------------------------------------------------------------------------
// layout measuring, for SuggestSlot
// ---------------------------------------------------------------------------

struct SNode
{
	xr_string	name;
	size_t		at;					// offset of '<'
	int			x, y, w, h;
};

// Direct children of <main_dialog>, with their rect. Enough of an XML scan for
// a game UI file: comments, declarations and self-closing tags are recognised,
// everything else is a plain open/close pair.
void ScanDialogChildren(const xr_string& ui, xr_vector<SNode>& out)
{
	const size_t root = ui.find("<main_dialog");
	if (root == xr_string::npos) return;
	size_t pos = ui.find('>', root);
	if (pos == xr_string::npos) return;
	++pos;

	int depth = 0;
	while (pos < ui.size())
	{
		const size_t lt = ui.find('<', pos);
		if (lt == xr_string::npos) break;
		pos = lt + 1;
		if (pos >= ui.size()) break;

		if (0 == ui.compare(pos, 3, "!--"))
		{
			const size_t end = ui.find("-->", pos);
			pos = (end == xr_string::npos) ? ui.size() : end + 3;
			continue;
		}
		if (ui[pos] == '?' || ui[pos] == '!')
		{
			const size_t end = ui.find('>', pos);
			pos = (end == xr_string::npos) ? ui.size() : end + 1;
			continue;
		}
		if (ui[pos] == '/')
		{
			if (0 == depth) break;			// </main_dialog>
			--depth;
			const size_t end = ui.find('>', pos);
			pos = (end == xr_string::npos) ? ui.size() : end + 1;
			continue;
		}

		size_t name_end = pos;
		while (name_end < ui.size() && (isalnum((u8)ui[name_end]) || ui[name_end] == '_')) ++name_end;
		const size_t close = ui.find('>', name_end);
		if (close == xr_string::npos) break;
		const bool self_closing = ui[close - 1] == '/';

		if (0 == depth)
		{
			SNode n;
			n.name	= ui.substr(pos, name_end - pos);
			n.at	= lt;
			n.x		= AttrInt(ui, lt, "x", -1);
			n.y		= AttrInt(ui, lt, "y", -1);
			n.w		= AttrInt(ui, lt, "width", -1);
			n.h		= AttrInt(ui, lt, "height", -1);
			out.push_back(n);
		}
		if (!self_closing) ++depth;
		pos = close + 1;
	}
}

// [provides_mode] of every XMS module installed in the linked game
// Modules live in <game>\modules\; <game>\mods\ is the folder JSGME manages and
// still holds anything installed before the split, so both are read.
static const char* kModuleRoots[] = { "modules", "mods" };

void ScanModuleRoot(LPCSTR game, LPCSTR root, xr_vector<EditorGameModes::SMode>& out)
{
	string_path mask;
	sprintf_s(mask, "%s\\%s\\*", game, root);
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (INVALID_HANDLE_VALUE == h) return;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))	continue;
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))	continue;

		string_path manifest;
		sprintf_s(manifest, "%s\\%s\\%s\\mod.ltx", game, root, fd.cFileName);
		HANDLE f = ::CreateFileA(manifest, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
								 FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == f) continue;
		LARGE_INTEGER size;
		xr_string text;
		if (::GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20))
		{
			xr_vector<char> buf(size_t(size.QuadPart) + 1, 0);
			DWORD read = 0;
			if (::ReadFile(f, buf.data(), DWORD(size.QuadPart), &read, NULL)) text = buf.data();
		}
		::CloseHandle(f);
		if (text.empty()) continue;

		// [provides_mode] carries id/title pairs; id opens a new entry
		const size_t sect = text.find("[provides_mode]");
		if (sect == xr_string::npos) continue;
		size_t pos = sect;
		EditorGameModes::SMode pending;
		while (pos < text.size())
		{
			const size_t eol = text.find('\n', pos);
			xr_string line = text.substr(pos, (eol == xr_string::npos ? text.size() : eol) - pos);
			pos = (eol == xr_string::npos) ? text.size() : eol + 1;
			if (line.find('[') != xr_string::npos && line.find("[provides_mode]") == xr_string::npos) break;
			const size_t eq = line.find('=');
			if (eq == xr_string::npos) continue;
			xr_string key = line.substr(0, eq), val = line.substr(eq + 1);
			while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
			while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
			while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();
			if (0 == _stricmp(key.c_str(), "id"))
			{
				if (!pending.id.empty()) out.push_back(pending);
				pending			= EditorGameModes::SMode();
				pending.id		= val;
				pending.caption	= val;
				pending.campaign= true;
				pending.source	= fd.cFileName;
			}
			else if (0 == _stricmp(key.c_str(), "title") && !pending.id.empty())
				pending.caption = CaptionFromManifest(val.c_str());
		}
		if (!pending.id.empty()) out.push_back(pending);
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
}

// [provides_mode] of every XMS module installed in the linked game
void ScanInstalledModules(xr_vector<EditorGameModes::SMode>& out)
{
	LPCSTR game = EditorProject::GameRoot();
	if (!game || !game[0]) return;

	const u32 was = (u32)out.size();
	for (int r = 0; r < 2; ++r)
	{
		xr_vector<EditorGameModes::SMode> found;
		ScanModuleRoot(game, kModuleRoots[r], found);
		// the same module in both roots declares its mode once
		for (u32 i = 0; i < found.size(); ++i)
		{
			bool dup = false;
			for (u32 j = was; j < out.size() && !dup; ++j)
				dup = (0 == _stricmp(out[j].id.c_str(), found[i].id.c_str()));
			if (!dup) out.push_back(found[i]);
		}
	}
}
} // namespace

void EditorGameModes::Invalidate()
{
	s_Scanned = false;
	s_Modes.clear();
	s_Error.clear();
}

bool EditorGameModes::Scan(xr_string& err)
{
	err.clear();
	if (s_Scanned) { err = s_Error; return s_Error.empty(); }
	s_Scanned = true;
	s_Modes.clear();

	xr_string mount_err;
	if (!EditorGameContent::EnsureMounted(mount_err))
	{
		s_Error = mount_err.empty() ? xr_string("no game linked") : mount_err;
		err = s_Error;
		return false;
	}

	// the 16:9 layout is what the game actually uses; the 4:3 one is a fallback
	xr_string ui = ReadGameText("configs\\ui\\ui_mm_faction_select_16.xml");
	if (ui.empty()) ui = ReadGameText("configs\\ui\\ui_mm_faction_select.xml");
	const xr_string script = ReadGameText("scripts\\ui_mm_faction_select.script");
	if (ui.empty())
	{
		s_Error = "the linked game has no ui_mm_faction_select xml - cannot tell modes from options";
		err = s_Error;
		ScanInstalledModules(s_Modes);
		return false;
	}

	// Column headers decide which kind a checkbox is. Without them (a stock
	// 0.98b install) nothing here is a campaign - the stock checkboxes are all
	// rule toggles, and the game itself calls them options.
	int x_mode = -1, x_option = -1;
	if (const size_t at = ui.find("<dar2_mode"); at != xr_string::npos)		x_mode   = AttrInt(ui, at, "x", -1);
	if (const size_t at = ui.find("<dar2_option"); at != xr_string::npos)	x_option = AttrInt(ui, at, "x", -1);

	size_t pos = 0;
	while ((pos = ui.find("<check_", pos)) != xr_string::npos)
	{
		const size_t name_at = pos + 1;
		size_t name_end = name_at;
		while (name_end < ui.size() && (isalnum((u8)ui[name_end]) || ui[name_end] == '_')) ++name_end;
		const xr_string node = ui.substr(name_at, name_end - name_at);
		const int x = AttrInt(ui, pos, "x", -1);
		pos = name_end;

		SMode m;
		m.source		= "game";
		// The checkbox node and the option key do not always match:
		// check_metro_mode pairs with new_game_metro_mode, but check_survival
		// pairs with new_game_survival_mode. Take whichever spelling the
		// script actually uses.
		{
			const xr_string base	= "new_game_" + node.substr(6);
			const xr_string with	= base + "_mode";
			m.config_key = (!script.empty() && script.find(with) != xr_string::npos) ? with : base;
		}
		m.id			= NormalizeId(m.config_key);
		m.available		= script.empty() ? true : CheckboxLive(script, node);

		// nearer column wins; with no headers at all nothing is a campaign
		if (x_mode >= 0 && x_option >= 0 && x >= 0)
			m.campaign = abs(x - x_mode) <= abs(x - x_option);
		else
			m.campaign = false;

		// the caption lives on the sibling label node, as a string-table id
		xr_string cap_tag = "<cap_";
		cap_tag += node;
		if (const size_t cap_at = ui.find(cap_tag); cap_at != xr_string::npos)
		{
			const size_t open = ui.find('>', cap_at);
			const size_t t_open = (open == xr_string::npos) ? xr_string::npos : ui.find('>', ui.find("<text", open));
			const size_t t_end  = (t_open == xr_string::npos) ? xr_string::npos : ui.find("</text>", t_open);
			if (t_open != xr_string::npos && t_end != xr_string::npos)
				m.caption = ui.substr(t_open + 1, t_end - t_open - 1);
		}
		s_Modes.push_back(m);
	}

	ResolveCaptions(s_Modes);

	// options are parsed only to be told apart; they never reach a caller
	xr_vector<SMode> kept;
	for (u32 i = 0; i < s_Modes.size(); ++i)
		if (s_Modes[i].campaign && s_Modes[i].available) kept.push_back(s_Modes[i]);
	s_Modes = kept;

	ScanInstalledModules(s_Modes);
	return true;
}

// Rows other installed modules already appended to this screen. Their patches
// are plain files under <game>\modules\<id>\patch\*.xmlp (and under the JSGME
// folder for anything installed before the split), so the answer is on disk:
// without this every module that adds a mode would compute the same row and
// the checkboxes would sit on top of each other.
static int RowsTakenInRoot(LPCSTR game, LPCSTR root, LPCSTR layout_file, int column_x, LPCSTR skip_module)
{
	int lowest = 0;
	string_path mask;
	sprintf_s(mask, "%s\\%s\\*", game, root);
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (INVALID_HANDLE_VALUE == h) return 0;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))	continue;
		if (fd.cFileName[0] == '.')								continue;
		if (skip_module && 0 == _stricmp(fd.cFileName, skip_module)) continue;

		string_path pmask;
		sprintf_s(pmask, "%s\\%s\\%s\\patch\\*.xmlp", game, root, fd.cFileName);
		WIN32_FIND_DATAA pf;
		HANDLE ph = ::FindFirstFileA(pmask, &pf);
		if (INVALID_HANDLE_VALUE == ph) continue;
		do
		{
			string_path file;
			sprintf_s(file, "%s\\%s\\%s\\patch\\%s", game, root, fd.cFileName, pf.cFileName);
			HANDLE f = ::CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
									 FILE_ATTRIBUTE_NORMAL, NULL);
			if (INVALID_HANDLE_VALUE == f) continue;
			xr_string text;
			LARGE_INTEGER size;
			if (::GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20))
			{
				xr_vector<char> buf(size_t(size.QuadPart) + 1, 0);
				DWORD read = 0;
				if (::ReadFile(f, buf.data(), DWORD(size.QuadPart), &read, NULL)) text = buf.data();
			}
			::CloseHandle(f);
			if (text.empty() || text.find(layout_file) == xr_string::npos) continue;

			size_t pos = 0;
			while ((pos = text.find("<check_", pos)) != xr_string::npos)
			{
				const int x = AttrInt(text, pos, "x", -1);
				const int y = AttrInt(text, pos, "y", -1);
				if (y > lowest && x >= 0 && abs(x - column_x) <= 4) lowest = y;
				pos += 7;
			}
		} while (::FindNextFileA(ph, &pf));
		::FindClose(ph);
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
	return lowest;
}

static int RowsTakenByModules(LPCSTR layout_file, int column_x, LPCSTR skip_module)
{
	LPCSTR game = EditorProject::GameRoot();
	if (!game || !game[0] || !layout_file) return 0;
	int lowest = 0;
	for (int r = 0; r < 2; ++r)
	{
		const int y = RowsTakenInRoot(game, kModuleRoots[r], layout_file, column_x, skip_module);
		if (y > lowest) lowest = y;
	}
	return lowest;
}

bool EditorGameModes::SuggestSlot(LPCSTR layout_rel, int rows, LPCSTR skip_module,
								  SSlot& out, xr_string& err)
{
	err.clear();
	out = SSlot();
	out.layout = layout_rel ? layout_rel : "";
	if (rows < 1) rows = 1;

	xr_string mount_err;
	if (!EditorGameContent::EnsureMounted(mount_err))
	{
		err = mount_err.empty() ? xr_string("no game linked") : mount_err;
		return false;
	}
	const xr_string ui = ReadGameText(layout_rel);
	if (ui.empty()) { err = "layout not found in the linked game"; return false; }

	xr_vector<SNode> nodes;
	ScanDialogChildren(ui, nodes);
	if (nodes.empty()) { err = "the layout has no main_dialog children"; return false; }

	// which column takes campaigns: the one the <dar2_mode> header sits over,
	// or - on a screen without headers - simply the leftmost checkbox column
	int x_mode = -1, x_option = -1;
	if (const size_t at = ui.find("<dar2_mode"); at != xr_string::npos)		x_mode   = AttrInt(ui, at, "x", -1);
	if (const size_t at = ui.find("<dar2_option"); at != xr_string::npos)	x_option = AttrInt(ui, at, "x", -1);

	xr_vector<const SNode*> checks;
	for (u32 i = 0; i < nodes.size(); ++i)
		if (0 == _strnicmp(nodes[i].name.c_str(), "check_", 6) && nodes[i].x >= 0 && nodes[i].y >= 0)
			checks.push_back(&nodes[i]);
	if (checks.empty()) { err = "the layout has no checkboxes to sit next to"; return false; }

	int column_x = checks[0]->x;
	for (u32 i = 1; i < checks.size(); ++i)
	{
		const int x = checks[i]->x;
		if (x_mode >= 0 && x_option >= 0)
		{
			const bool campaign	= abs(x - x_mode) <= abs(x - x_option);
			const bool have		= abs(column_x - x_mode) <= abs(column_x - x_option);
			if (campaign && !have) column_x = x;			// first campaign column wins
		}
		else if (x < column_x)
			column_x = x;									// no headers: leftmost
	}

	// The row itself: same geometry as the column's last checkbox, one step
	// down. The step is the SMALLEST gap between rows, not the largest - a
	// column with a visual break in it would otherwise push the new row off
	// the dialog entirely.
	const SNode* last = 0;
	int step = 0;
	for (u32 i = 0; i < checks.size(); ++i)
	{
		if (abs(checks[i]->x - column_x) > 4) continue;
		if (!last || checks[i]->y > last->y)
		{
			const int gap = last ? (checks[i]->y - last->y) : 0;
			if (gap > 0 && (step <= 0 || gap < step)) step = gap;
			last = checks[i];
		}
	}
	if (!last) { err = "the campaign column is empty"; return false; }
	if (step <= 0) step = (last->h > 0 ? last->h : 21) + 4;
	out.step = step;

	out.check_x	= last->x;
	out.check_w	= last->w > 0 ? last->w : 30;
	out.check_h	= last->h > 0 ? last->h : 21;

	// start below whatever is already there, base layout or another module
	const char* layout_file = strrchr(out.layout.c_str(), '\\');
	layout_file = layout_file ? layout_file + 1 : out.layout.c_str();
	const int taken = RowsTakenByModules(layout_file, out.check_x, skip_module);
	out.check_y	= (taken > last->y ? taken : last->y) + step;

	// The label: the sibling cap_ node of that same checkbox, when there is one.
	// Its y is kept as an OFFSET from the checkbox - the two are not always
	// flush (the 4:3 layout sets the label 5px lower), and copying the offset
	// keeps the new row looking like every other one.
	out.cap_x	= out.check_x + out.check_w;
	out.cap_y	= out.check_y;
	xr_string cap_name = "cap_" + last->name;
	for (u32 i = 0; i < nodes.size(); ++i)
		if (0 == _stricmp(nodes[i].name.c_str(), cap_name.c_str()))
		{
			if (nodes[i].x > 0) out.cap_x = nodes[i].x;
			if (nodes[i].w > 0) out.cap_w = nodes[i].w;
			if (nodes[i].h > 0) out.cap_h = nodes[i].h;
			if (nodes[i].y >= 0) out.cap_y = out.check_y + (nodes[i].y - last->y);
			break;
		}

	// The frame around the column: the smallest node that holds the last row.
	// Growing it keeps the added checkbox inside the border instead of under
	// it. The containment test is fuzzy on purpose - in the stock layout the
	// checkbox column pokes a few pixels out of its own frame, and an exact
	// test would then pick the outer frame and grow the wrong thing.
	const int kSlack = 8;
	const SNode* frame = 0;
	for (u32 i = 0; i < nodes.size(); ++i)
	{
		const SNode& n = nodes[i];
		if (n.x < 0 || n.y < 0 || n.w <= 0 || n.h <= 0)					continue;
		if (0 == _strnicmp(n.name.c_str(), "check_", 6))					continue;
		if (0 == _strnicmp(n.name.c_str(), "cap_", 4))					continue;
		if (n.x > last->x + kSlack || n.y > last->y + kSlack)			continue;
		if (n.x + n.w + kSlack < last->x + out.check_w)					continue;
		if (n.y + n.h + kSlack < last->y + out.check_h)					continue;
		if (!frame || (n.w * n.h) < (frame->w * frame->h))				frame = &n;
	}

	const int row_bottom = out.check_y + (rows - 1) * step + out.check_h;
	if (frame && frame->y + frame->h < row_bottom + 4)
	{
		const int grown	= row_bottom + 4 - frame->y;
		const int delta	= grown - frame->h;
		out.frame_node		= frame->name;
		out.frame_height	= grown;

		// everything that started below the frame moves down with it
		for (u32 i = 0; i < nodes.size(); ++i)
		{
			const SNode& n = nodes[i];
			if (&n == frame || n.y < frame->y + frame->h)	continue;
			SSlotShift s;
			s.node	= n.name;
			s.y		= n.y + delta;
			out.shift.push_back(s);
		}
	}
	return true;
}

const xr_vector<EditorGameModes::SMode>& EditorGameModes::All()
{
	return s_Modes;
}

void EditorGameModes::McpList(xr_string& out)
{
	xr_string err;
	const bool ok = Scan(err);
	out  = ok ? "{\"ok\":true" : "{\"ok\":false";
	if (!ok) { out += ",\"error\":\""; EditorGameContent::JsonAppendPath(out, err.c_str()); out += "\""; }
	out += ",\"modes\":[";
	const xr_vector<SMode>& all = All();
	for (u32 i = 0; i < all.size(); ++i)
	{
		if (i) out += ",";
		out += "{\"id\":\"";		EditorGameContent::JsonAppendPath(out, all[i].id.c_str());
		out += "\",\"caption\":\"";	EditorGameContent::JsonAppendPath(out, all[i].caption.c_str());
		out += "\",\"key\":\"";		EditorGameContent::JsonAppendPath(out, all[i].config_key.c_str());
		out += "\",\"source\":\"";	EditorGameContent::JsonAppendPath(out, all[i].source.c_str());
		out += "\"}";
	}
	out += "]}";
}
