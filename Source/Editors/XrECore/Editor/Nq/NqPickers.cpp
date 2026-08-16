#include "stdafx.h"
#include "NqPickers.h"
#include "NqUtil.h"
#include "NqDoc.h"
#include "NqValidate.h"
#include "../EditorGameContent.h"
#include "../EditorGameConfigs.h"
#include "../EditorProject.h"

namespace
{
	xr_vector<NqPickers::SEntry>	s_Index[NqPickers::tCount];
	xr_set<xr_string>				s_Known[NqPickers::tCount];
	bool							s_Built		= false;
	bool							s_Available	= false;
	xr_string						s_BuiltFor;			// fingerprint + project root
	xr_string						s_ProjectRoot;

	const char* kTypeNames[NqPickers::tCount] =
	{
		"item_section", "squad_section", "level", "smart", "story_id", "profile", "community", "info", "spot_type",
	};

	xr_string CurrentKey()
	{
		xr_string k = EditorGameConfigs::ActiveFingerprint();
		k += "|";
		k += (EditorProject::Active() ? EditorProject::Root() : "");
		return k;
	}

	void Add(NqPickers::EType t, const xr_string& id, const xr_string& name, const xr_string& extra)
	{
		if (id.empty()) return;
		if (s_Known[t].count(id)) return;
		s_Known[t].insert(id);
		NqPickers::SEntry e; e.id = id; e.extra = extra;
		// the UI prints "id - name", so a caption repeating the id is pure noise
		if (name != id) e.name = name;
		s_Index[t].push_back(e);
	}

	xr_string ReadGameText(LPCSTR rel)
	{
		xr_string text;
		const int idx = EditorGameContent::Find(rel);
		if (idx < 0) return text;
		u32 size = 0;
		u8* bytes = EditorGameContent::ReadBytes(idx, size);
		if (!bytes) return text;
		text.assign((const char*)bytes, (const char*)bytes + size);
		EditorGameContent::FreeBytes(bytes);
		return text;
	}

	// ---- string table (id -> cp1251 text) --------------------------------------
	typedef xr_map<xr_string, xr_string> TStrings;

	void LoadStringTable(TStrings& out)
	{
		// language from localization.ltx, rus by default
		xr_string lang = "rus";
		{
			xr_string loc = ReadGameText("configs\\localization.ltx");
			size_t at = loc.find("language");
			if (at != xr_string::npos)
			{
				size_t eq = loc.find('=', at);
				size_t eol = loc.find('\n', at);
				if (eq != xr_string::npos && (eol == xr_string::npos || eq < eol))
				{
					xr_string v = NqUtil::Trim(loc.substr(eq + 1, (eol == xr_string::npos ? loc.size() : eol) - eq - 1));
					const size_t sc = v.find(';');
					if (sc != xr_string::npos) v = NqUtil::Trim(v.substr(0, sc));
					if (!v.empty()) lang = v;
				}
			}
		}
		string_path prefix;
		sprintf_s(prefix, "configs\\text\\%s\\", lang.c_str());
		const size_t plen = strlen(prefix);
		const int total = EditorGameContent::Count();
		for (int i = 0; i < total; ++i)
		{
			const EditorGameContent::SItem* it = EditorGameContent::Get(i);
			if (!it || 0 != _strnicmp(it->path.c_str(), prefix, plen)) continue;
			if (it->path.size() < 4 || 0 != _stricmp(it->path.c_str() + it->path.size() - 4, ".xml")) continue;
			u32 size = 0;
			u8* bytes = EditorGameContent::ReadBytes(i, size);
			if (!bytes) continue;
			xr_string text; text.assign((const char*)bytes, (const char*)bytes + size);
			EditorGameContent::FreeBytes(bytes);
			// <string id="X"> ... <text>Y</text>
			size_t pos = 0;
			for (;;)
			{
				const size_t at = text.find("<string id=\"", pos);
				if (at == xr_string::npos) break;
				const size_t idb = at + 12;
				const size_t ide = text.find('"', idb);
				if (ide == xr_string::npos) break;
				const size_t open = text.find("<text>", ide);
				const size_t next = text.find("<string id=\"", ide);
				pos = ide;
				if (open == xr_string::npos || (next != xr_string::npos && open > next)) continue;
				const size_t close = text.find("</text>", open);
				if (close == xr_string::npos) break;
				xr_string id = text.substr(idb, ide - idb);
				if (!out.count(id)) out[id] = text.substr(open + 6, close - open - 6);
				pos = close;
			}
		}
	}

	xr_string Caption(const TStrings& st, const xr_string& id)
	{
		TStrings::const_iterator it = st.find(id);
		if (it == st.end()) return xr_string();
		return NqUtil::Cp1251ToUtf8(NqUtil::Trim(it->second).c_str());
	}

	// ---- builders ----------------------------------------------------------------
	// `profiles` maps a character profile id to its caption (see BuildProfiles), so
	// a story id sitting on an NPC spawn section can borrow the NPC's name.
	void BuildFromSettings(const TStrings& st, const TStrings& profiles)
	{
		if (!pSettings) return;
		CInifile::Root& sects = pSettings->sections();
		for (CInifile::RootIt s = sects.begin(); s != sects.end(); ++s)
		{
			CInifile::Sect* S = *s;
			if (!S) continue;
			LPCSTR v = 0;
			xr_string inv_caption;		// genuine item caption, reused by a story id here
			if (S->line_exist("inv_name", &v))
			{
				xr_string key = v ? v : "";
				inv_caption = Caption(st, key);
				xr_string name = inv_caption.empty() ? NqUtil::Cp1251ToUtf8(key.c_str()) : inv_caption;
				Add(NqPickers::tItem, xr_string(*S->Name), name, xr_string());
			}
			if (S->line_exist("class", &v) && v && 0 == _stricmp(v, "ON_OFF_S"))
				Add(NqPickers::tSquad, xr_string(*S->Name), xr_string(), xr_string());
			if (S->line_exist("story_id", &v) && v && v[0])
			{
				const xr_string sid = v;		// the lookups below reuse `v`
				xr_string extra = "section ";
				extra += *S->Name;
				// a story id is attached to a spawn section: NPCs read through their
				// character profile, items through inv_name. Squads and bare physical
				// objects carry no player-facing text and stay uncaptioned.
				xr_string name;
				LPCSTR prof = 0;
				if (S->line_exist("character_profile", &prof) && prof && prof[0])
				{
					TStrings::const_iterator it = profiles.find(xr_string(prof));
					if (it != profiles.end()) name = it->second;
				}
				if (name.empty()) name = inv_caption;
				// a squad whose roster is one named NPC reads as that NPC; a roster
				// of generic templates ("npc = sim_default_stalker_0, ...") does not
				if (name.empty() && S->line_exist("npc", &prof) && prof && prof[0] && !strchr(prof, ','))
				{
					const xr_string npc = NqUtil::Trim(prof);
					if (pSettings->line_exist(npc.c_str(), "character_profile"))
					{
						LPCSTR np = pSettings->r_string(npc.c_str(), "character_profile");
						TStrings::const_iterator it = (np && np[0]) ? profiles.find(xr_string(np)) : profiles.end();
						if (it != profiles.end()) name = it->second;
					}
				}
				Add(NqPickers::tStory, sid, name, extra);
			}
		}
	}

	void BuildLevels(const TStrings& st)
	{
		xr_string text = ReadGameText("configs\\game_levels.ltx");
		if (text.empty()) return;
		// the displayed name lives in the string table under the level name itself
		// (st_levels.xml keys on "l01_escape"); the ltx `caption` is a second key
		auto resolve = [&st](const xr_string& lvl, const xr_string& key) -> xr_string
		{
			xr_string out = Caption(st, lvl);
			if (out.empty() && !key.empty()) out = Caption(st, key);
			if (out.empty()) out = NqUtil::Cp1251ToUtf8(key.c_str());
			return out;
		};
		xr_string sect, name, caption;
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t eol = text.find('\n', pos);
			if (eol == xr_string::npos) eol = text.size();
			xr_string line = NqUtil::Trim(text.substr(pos, eol - pos));
			pos = eol + 1;
			const size_t sc = line.find(';');
			if (sc != xr_string::npos) line = NqUtil::Trim(line.substr(0, sc));
			if (line.empty()) continue;
			if (line[0] == '[')
			{
				if (!name.empty() && sect != "levels") Add(NqPickers::tLevel, name, resolve(name, caption), sect);
				const size_t close = line.find(']');
				sect = line.substr(1, close == xr_string::npos ? xr_string::npos : close - 1);
				name.clear(); caption.clear();
				continue;
			}
			const size_t eq = line.find('=');
			if (eq == xr_string::npos) continue;
			xr_string k = NqUtil::Trim(line.substr(0, eq)), v = NqUtil::Trim(line.substr(eq + 1));
			if (v.size() >= 2 && v[0] == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
			if (k == "name")			name = v;
			else if (k == "caption")	caption = v;
		}
		if (!name.empty() && sect != "levels") Add(NqPickers::tLevel, name, resolve(name, caption), sect);
	}

	void BuildSmarts(const TStrings& st)
	{
		// misc\smart_names.ltx maps "<smart> = <string table id>" inside a section
		// per level, exactly what smart_names.script reads. Only a few levels are
		// covered; every other smart terrain legitimately has no display name.
		TStrings keys;
		{
			xr_string names = ReadGameText("configs\\misc\\smart_names.ltx");
			size_t pos = 0;
			bool in_levels = false;
			while (pos < names.size())
			{
				size_t eol = names.find('\n', pos);
				if (eol == xr_string::npos) eol = names.size();
				xr_string line = NqUtil::Trim(names.substr(pos, eol - pos));
				pos = eol + 1;
				const size_t sc = line.find(';');
				if (sc != xr_string::npos) line = NqUtil::Trim(line.substr(0, sc));
				if (line.empty()) continue;
				if (line[0] == '[') { in_levels = (0 == _strnicmp(line.c_str(), "[levels]", 8)); continue; }
				if (in_levels) continue;		// that section only lists level names
				const size_t eq = line.find('=');
				if (eq == xr_string::npos) continue;
				const xr_string smart = NqUtil::Trim(line.substr(0, eq));
				if (!smart.empty()) keys[smart] = NqUtil::Trim(line.substr(eq + 1));
			}
		}
		xr_string text = ReadGameText("configs\\misc\\simulation.ltx");
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t eol = text.find('\n', pos);
			if (eol == xr_string::npos) eol = text.size();
			xr_string line = NqUtil::Trim(text.substr(pos, eol - pos));
			pos = eol + 1;
			if (line.empty() || line[0] != '[') continue;
			const size_t close = line.find(']');
			if (close == xr_string::npos) continue;
			const xr_string id = line.substr(1, close - 1);
			TStrings::const_iterator k = keys.find(id);
			Add(NqPickers::tSmart, id, (k == keys.end()) ? xr_string() : Caption(st, k->second), xr_string());
		}
	}

	void BuildCommunities(const TStrings& st)
	{
		xr_string text = ReadGameText("configs\\creatures\\game_relations.ltx");
		size_t pos = 0;
		xr_string sect;
		while (pos < text.size())
		{
			size_t eol = text.find('\n', pos);
			if (eol == xr_string::npos) eol = text.size();
			xr_string line = NqUtil::Trim(text.substr(pos, eol - pos));
			pos = eol + 1;
			const size_t sc = line.find(';');
			if (sc != xr_string::npos) line = NqUtil::Trim(line.substr(0, sc));
			if (line.empty()) continue;
			if (line[0] == '[') { const size_t c = line.find(']'); sect = line.substr(1, c == xr_string::npos ? xr_string::npos : c - 1); continue; }
			if (sect != "communities" && sect != "monster_communities" && sect != "game_relations") continue;
			const size_t eq = line.find('=');
			if (eq == xr_string::npos) continue;
			if (NqUtil::Trim(line.substr(0, eq)) != "communities") continue;
			xr_vector<xr_string> parts;
			NqUtil::Split(line.substr(eq + 1).c_str(), ',', parts);
			// name, index, name, index, ...
			for (u32 i = 0; i + 1 < parts.size(); i += 2)
			{
				// the faction picker labels a community in the plural
				// (st_faction_stalker); the bare id is the singular PDA label
				xr_string name = Caption(st, xr_string("st_faction_") + parts[i]);
				if (name.empty()) name = Caption(st, parts[i]);
				Add(NqPickers::tCommunity, parts[i], name, sect);
			}
		}
	}

	// gameplay\<file>.xml, tag <specific_character id="..."> or <info_portion id="...">
	void ScanIds(const xr_string& text, LPCSTR tag, xr_vector<std::pair<xr_string, size_t> >& out)
	{
		xr_string pat = "<";
		pat += tag;
		size_t pos = 0;
		for (;;)
		{
			const size_t at = text.find(pat, pos);
			if (at == xr_string::npos) break;
			pos = at + pat.size();
			const char after = (pos < text.size()) ? text[pos] : 0;
			if (after != ' ' && after != '\t' && after != '\n' && after != '\r') continue;
			const size_t gt = text.find('>', at);
			const size_t idat = text.find("id=\"", at);
			if (idat == xr_string::npos || (gt != xr_string::npos && idat > gt)) continue;
			const size_t ide = text.find('"', idat + 4);
			if (ide == xr_string::npos) break;
			out.push_back(std::make_pair(text.substr(idat + 4, ide - idat - 4), at));
		}
	}

	// `captions` collects profile id -> caption for the story-id pass. Only genuine
	// string-table hits go in, so a generic profile whose <name> is a runtime
	// placeholder (GENERATE_NAME_stalker) does not leak into story id captions.
	void BuildProfiles(const TStrings& st, TStrings& captions)
	{
		if (!pSettings || !pSettings->section_exist("profiles") || !pSettings->line_exist("profiles", "specific_characters_files")) return;
		xr_vector<xr_string> files;
		NqUtil::Split(pSettings->r_string("profiles", "specific_characters_files"), ',', files);
		for (u32 f = 0; f < files.size(); ++f)
		{
			string_path rel;
			sprintf_s(rel, "configs\\gameplay\\%s.xml", files[f].c_str());
			xr_string text = ReadGameText(rel);
			if (text.empty()) continue;
			xr_vector<std::pair<xr_string, size_t> > ids;
			ScanIds(text, "specific_character", ids);
			for (u32 i = 0; i < ids.size(); ++i)
			{
				// <name>string_id</name> right after the tag; the string table turns it into a caption
				xr_string name;
				const size_t nm = text.find("<name>", ids[i].second);
				const size_t next = (i + 1 < ids.size()) ? ids[i + 1].second : text.size();
				if (nm != xr_string::npos && nm < next)
				{
					const size_t ne = text.find("</name>", nm);
					if (ne != xr_string::npos)
					{
						xr_string key = NqUtil::Trim(text.substr(nm + 6, ne - nm - 6));
						name = Caption(st, key);
						if (!name.empty() && !captions.count(ids[i].first)) captions[ids[i].first] = name;
						else if (name.empty()) name = NqUtil::Cp1251ToUtf8(key.c_str());
					}
				}
				Add(NqPickers::tProfile, ids[i].first, name, files[f]);
			}
		}
	}

	void BuildInfos()
	{
		if (!pSettings || !pSettings->section_exist("info_portions") || !pSettings->line_exist("info_portions", "files")) return;
		xr_vector<xr_string> files;
		NqUtil::Split(pSettings->r_string("info_portions", "files"), ',', files);
		for (u32 f = 0; f < files.size(); ++f)
		{
			string_path rel;
			sprintf_s(rel, "configs\\gameplay\\%s.xml", files[f].c_str());
			xr_string text = ReadGameText(rel);
			if (text.empty()) continue;
			xr_vector<std::pair<xr_string, size_t> > ids;
			ScanIds(text, "info_portion", ids);
			for (u32 i = 0; i < ids.size(); ++i) Add(NqPickers::tInfo, ids[i].first, xr_string(), files[f]);
		}
	}

	void BuildSpots()
	{
		static const char* kSpots[] =
		{
			"secondary_task_location", "storyline_task_location", "ui_secondary_task_blink",
			"ui_storyline_task_blink", "treasure", "primary_task_location",
		};
		for (int i = 0; i < (int)(sizeof(kSpots) / sizeof(kSpots[0])); ++i)
			Add(NqPickers::tSpot, xr_string(kSpots[i]), xr_string(), xr_string("core"));
	}

	// Caption of a spawn section: NPCs read through their character profile, items
	// through inv_name. Anything else (restrictors, smarts, physical props) simply
	// has no player-facing text and must stay bare rather than echo an id.
	xr_string SectionCaption(const xr_string& sect, const TStrings& st, const TStrings& profiles)
	{
		if (!pSettings || sect.empty() || !pSettings->section_exist(sect.c_str())) return xr_string();
		if (pSettings->line_exist(sect.c_str(), "character_profile"))
		{
			LPCSTR prof = pSettings->r_string(sect.c_str(), "character_profile");
			if (prof && prof[0])
			{
				TStrings::const_iterator it = profiles.find(xr_string(prof));
				if (it != profiles.end()) return it->second;
			}
		}
		if (pSettings->line_exist(sect.c_str(), "inv_name"))
		{
			LPCSTR key = pSettings->r_string(sect.c_str(), "inv_name");
			if (key && key[0]) return Caption(st, xr_string(key));
		}
		return xr_string();
	}

	// Story ids of the objects the game actually placed live in the compiled spawn,
	// not in the config sections: a `story_id` line there belongs to a RESPAWN
	// TEMPLATE. That is why the picker used to offer esc_2_12_stalker_trader, which
	// resolves to nothing at runtime, and hide esc_m_trader, which is what the
	// game's own tasks target. The custom data rides inside all.spawn as plain
	// text, so the block is read straight out of the bytes.
	void BuildSpawnStories(const TStrings& st, const TStrings& profiles)
	{
		const int idx = EditorGameContent::Find("spawns\\all.spawn");
		if (idx < 0) return;
		u32 size = 0;
		u8* bytes = EditorGameContent::ReadBytes(idx, size);
		if (!bytes) return;

		const char* kMark = "[story_object]";
		const size_t mark_len = strlen(kMark);
		const char* p = (const char*)bytes;
		const char* end = p + size;
		while (p + mark_len < end)
		{
			const char* at = (const char*)memchr(p, '[', end - p - mark_len);
			if (!at) break;
			if (0 != memcmp(at, kMark, mark_len)) { p = at + 1; continue; }

			// the value is on one of the next few lines: normally "story_id = x",
			// but at least one shipped object writes the bare id with no key
			const char* q = at + mark_len;
			const char* stop = (q + 256 < end) ? q + 256 : end;
			while (q < stop)
			{
				const char* eol = q;
				while (eol < stop && *eol != '\n' && *eol != '\r' && *eol) ++eol;
				xr_string raw;
				raw.assign(q, eol);
				xr_string line = NqUtil::Trim(raw.c_str());
				q = (eol < stop) ? eol + 1 : stop;
				if (line.empty()) continue;
				if (line[0] == '[') break;					// next section, nothing here
				const size_t eq = line.find('=');
				xr_string sid;
				if (eq != xr_string::npos)
				{
					if (NqUtil::Trim(line.substr(0, eq)) != "story_id") break;
					sid = NqUtil::Trim(line.substr(eq + 1));
				}
				else sid = line;
				if (!sid.empty())
				{
					// The packet just before the block names the object's spawn section
					// (its logic cfg path carries it), and that section is what knows the
					// NPC's profile - which is how esc_m_trader reads as its character
					// instead of as an id. Only a section that actually has a caption is
					// accepted, so a stray token cannot invent one.
					xr_string name, from;
					// the section shows up on both sides: the packet before the block, and
					// the "[logic] cfg = scripts\<level>\<section>.ltx" line just after it
					const char* back = (at - 512 > (const char*)bytes) ? at - 512 : (const char*)bytes;
					const char* fwd = (at + 512 < end) ? at + 512 : end;
					// candidates are ranked by how close they sit to the block, so the
					// object's own section wins over whatever the neighbouring packet held
					xr_vector<std::pair<size_t, xr_string> > tokens;
					xr_string tok;
					const char* tok_at = back;
					for (const char* b = back; b <= fwd; ++b)
					{
						const char c = (b < fwd) ? *b : ' ';
						const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
							|| (c >= '0' && c <= '9') || c == '_';
						if (word) { if (tok.empty()) tok_at = b; tok += c; continue; }
						if (tok.size() >= 4)
						{
							const size_t d = (tok_at < at) ? size_t(at - tok_at) : size_t(tok_at - at);
							tokens.push_back(std::make_pair(d, tok));
						}
						tok.clear();
					}
					std::sort(tokens.begin(), tokens.end());
					for (u32 t = 0; t < tokens.size() && name.empty(); ++t)
					{
						name = SectionCaption(tokens[t].second, st, profiles);
						if (!name.empty()) from = tokens[t].second;
					}
					xr_string extra = "spawn";
					if (!from.empty()) { extra += " "; extra += from; }
					Add(NqPickers::tStory, sid, name, extra);
				}
				break;
			}
			p = at + mark_len;
		}
		EditorGameContent::FreeBytes(bytes);
	}

	// [story_object] story_id = ... inside the project's spawn\custom_data\*.ltx
	void BuildProjectStories()
	{
		if (!EditorProject::Active()) return;
		string_path dir;
		sprintf_s(dir, "%s\\spawn\\custom_data", EditorProject::Root());
		xr_vector<xr_string> files;
		NqUtil::ListFiles(dir, ".ltx", files);
		for (u32 f = 0; f < files.size(); ++f)
		{
			xr_string text, err;
			HANDLE h = ::CreateFileA(files[f].c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (INVALID_HANDLE_VALUE == h) continue;
			const DWORD size = ::GetFileSize(h, NULL);
			if (size && size != INVALID_FILE_SIZE && size < (1 << 20))
			{
				xr_vector<char> buf(size + 1, 0);
				DWORD got = 0;
				if (::ReadFile(h, buf.data(), size, &got, NULL)) text.assign(buf.data(), buf.data() + got);
			}
			::CloseHandle(h);
			bool in_story = false;
			size_t pos = 0;
			while (pos < text.size())
			{
				size_t eol = text.find('\n', pos);
				if (eol == xr_string::npos) eol = text.size();
				xr_string line = NqUtil::Trim(text.substr(pos, eol - pos));
				pos = eol + 1;
				const size_t sc = line.find(';');
				if (sc != xr_string::npos) line = NqUtil::Trim(line.substr(0, sc));
				if (line.empty()) continue;
				if (line[0] == '[') { in_story = (0 == _strnicmp(line.c_str(), "[story_object]", 14)); continue; }
				if (!in_story) continue;
				const size_t eq = line.find('=');
				if (eq == xr_string::npos) continue;
				if (NqUtil::Trim(line.substr(0, eq)) != "story_id") continue;
				Add(NqPickers::tStory, NqUtil::Trim(line.substr(eq + 1)), xr_string(), xr_string("project"));
			}
		}
	}

	// ---- disk cache ----------------------------------------------------------------
	// bump the number on every change to what an entry stores, or caches written by
	// an older editor come back with the fields it did not know how to fill
	const char* kCacheFormat = "nqindex 6 ";

	void CachePath(string_path& out)
	{
		sprintf_s(out, "%s\\nq_index_%08x.txt", NqUtil::AppDataRoot(), NqUtil::Fnv1a(EditorGameConfigs::ActiveFingerprint()));
	}

	bool LoadCache()
	{
		string_path path;
		CachePath(path);
		HANDLE h = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == h) return false;
		xr_string text;
		const DWORD size = ::GetFileSize(h, NULL);
		if (size && size != INVALID_FILE_SIZE && size < (64 << 20))
		{
			xr_vector<char> buf(size + 1, 0);
			DWORD got = 0;
			if (::ReadFile(h, buf.data(), size, &got, NULL)) text.assign(buf.data(), buf.data() + got);
		}
		::CloseHandle(h);
		if (text.empty()) return false;
		// header line: "nqindex <format> <fingerprint>" - the format number is bumped
		// whenever the stored fields change, so an older cache is rebuilt, not read
		size_t eol = text.find('\n');
		if (eol == xr_string::npos) return false;
		xr_string head = text.substr(0, eol);
		xr_string want = kCacheFormat;
		want += EditorGameConfigs::ActiveFingerprint();
		if (NqUtil::Trim(head) != want) return false;
		int t = -1;
		size_t pos = eol + 1;
		while (pos < text.size())
		{
			eol = text.find('\n', pos);
			if (eol == xr_string::npos) eol = text.size();
			xr_string line = text.substr(pos, eol - pos);
			pos = eol + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty()) continue;
			if (line[0] == '[')
			{
				t = -1;
				for (int i = 0; i < NqPickers::tCount; ++i)
					if (line == xr_string("[") + kTypeNames[i] + "]") t = i;
				continue;
			}
			if (t < 0) continue;
			xr_string id, name, extra;
			const size_t t1 = line.find('\t');
			const size_t t2 = (t1 == xr_string::npos) ? xr_string::npos : line.find('\t', t1 + 1);
			id = line.substr(0, t1);
			if (t1 != xr_string::npos) name = line.substr(t1 + 1, (t2 == xr_string::npos ? line.size() : t2) - t1 - 1);
			if (t2 != xr_string::npos) extra = line.substr(t2 + 1);
			Add((NqPickers::EType)t, id, name, extra);
		}
		return true;
	}

	void SaveCache()
	{
		string_path path;
		CachePath(path);
		xr_string text = kCacheFormat;
		text += EditorGameConfigs::ActiveFingerprint();
		text += "\n";
		for (int t = 0; t < NqPickers::tCount; ++t)
		{
			text += "["; text += kTypeNames[t]; text += "]\n";
			for (u32 i = 0; i < s_Index[t].size(); ++i)
			{
				const NqPickers::SEntry& e = s_Index[t][i];
				if (e.extra == "project") continue;		// project entries are rebuilt each time
				text += e.id; text += "\t"; text += e.name; text += "\t"; text += e.extra; text += "\n";
			}
		}
		HANDLE h = ::CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == h) return;
		DWORD wr = 0;
		::WriteFile(h, text.c_str(), (DWORD)text.size(), &wr, NULL);
		::CloseHandle(h);
	}

	void Build()
	{
		for (int t = 0; t < NqPickers::tCount; ++t) { s_Index[t].clear(); s_Known[t].clear(); }
		s_Built = true;
		s_BuiltFor = CurrentKey();
		s_Available = false;

		xr_string mount_err;
		const bool game_configs = EditorGameConfigs::ActiveFingerprint() && EditorGameConfigs::ActiveFingerprint()[0];
		if (!EditorProject::Active() || !EditorProject::GameLinked() || !game_configs ||
			!EditorGameContent::EnsureMounted(mount_err))
		{
			// no game: only the fixed lists exist, and W060 stays quiet
			BuildSpots();
			return;
		}
		s_Available = true;
		if (!LoadCache())
		{
			TStrings st, profiles;
			LoadStringTable(st);
			BuildProfiles(st, profiles);	// must precede the story ids that borrow from it
			// placed objects first: a template id looks identical in the list but
			// resolves to nothing at runtime, so the entry that works is offered first
			BuildSpawnStories(st, profiles);
			BuildFromSettings(st, profiles);
			BuildLevels(st);
			BuildSmarts(st);
			BuildCommunities(st);
			BuildInfos();
			BuildSpots();
			for (int t = 0; t < NqPickers::tCount; ++t)
				std::sort(s_Index[t].begin(), s_Index[t].end(),
						  [](const NqPickers::SEntry& a, const NqPickers::SEntry& b) { return a.id < b.id; });
			SaveCache();
			Msg("* nq: game index built (%d items, %d squads, %d levels, %d smarts, %d story ids, %d profiles, %d infos)",
				(int)s_Index[NqPickers::tItem].size(), (int)s_Index[NqPickers::tSquad].size(),
				(int)s_Index[NqPickers::tLevel].size(), (int)s_Index[NqPickers::tSmart].size(),
				(int)s_Index[NqPickers::tStory].size(), (int)s_Index[NqPickers::tProfile].size(),
				(int)s_Index[NqPickers::tInfo].size());
		}
		BuildProjectStories();
	}

	void Ensure()
	{
		if (!s_Built || s_BuiltFor != CurrentKey()) Build();
	}
}

//------------------------------------------------------------------------------
bool NqPickers::Available()		{ Ensure(); return s_Available; }
void NqPickers::Invalidate()	{ s_Built = false; }

const xr_vector<NqPickers::SEntry>& NqPickers::Index(EType t)
{
	Ensure();
	static xr_vector<SEntry> empty;
	return (t >= 0 && t < tCount) ? s_Index[t] : empty;
}

bool NqPickers::Known(EType t, LPCSTR id)
{
	Ensure();
	if (t < 0 || t >= tCount || !id) return false;
	return s_Known[t].count(xr_string(id)) != 0;
}

NqPickers::EType NqPickers::TypeFromName(LPCSTR type_name)
{
	if (!type_name) return tCount;
	for (int i = 0; i < tCount; ++i) if (0 == strcmp(type_name, kTypeNames[i])) return (EType)i;
	return tCount;
}

LPCSTR NqPickers::TypeName(EType t)	{ return (t >= 0 && t < tCount) ? kTypeNames[t] : ""; }

// Captions are UTF-8 Russian, and _strnicmp folds ASCII only: it matches
// "Сидорович" but not "сидорович", which reads as "the search is broken".
// Cyrillic is two bytes, so it is folded by hand - А..Я -> а..я (А..П keep the
// D0 lead, Р..Я cross into D1) and Ё -> ё.
static void FoldNoCase(const xr_string& src, xr_string& out)
{
	out.clear();
	out.reserve(src.size());
	for (const u8* p = (const u8*)src.c_str(); *p; ++p)
	{
		const u8 c = *p;
		if (c >= 'A' && c <= 'Z') { out += char(c + 0x20); continue; }
		if (c == 0xD0 && p[1])
		{
			const u8 d = p[1];
			if (d == 0x81)				{ out += char(0xD1); out += char(0x91); ++p; continue; }
			if (d >= 0x90 && d <= 0x9F)	{ out += char(0xD0); out += char(d + 0x20); ++p; continue; }
			if (d >= 0xA0 && d <= 0xAF)	{ out += char(0xD1); out += char(d - 0x20); ++p; continue; }
		}
		out += char(c);
	}
}

static bool ContainsNoCase(const xr_string& hay, const xr_string& needle)
{
	if (needle.empty()) return true;
	if (hay.size() < needle.size()) return false;
	xr_string h, n;
	FoldNoCase(hay, h);
	FoldNoCase(needle, n);
	return 0 != strstr(h.c_str(), n.c_str());
}

void NqPickers::Search(EType t, LPCSTR query, int limit, xr_vector<const SEntry*>& out)
{
	out.clear();
	const xr_vector<SEntry>& idx = Index(t);
	if (limit <= 0) limit = 100;
	xr_string q = query ? query : "";
	// prefix matches first, then substring matches
	for (int pass = 0; pass < 2 && (int)out.size() < limit; ++pass)
		for (u32 i = 0; i < idx.size() && (int)out.size() < limit; ++i)
		{
			const SEntry& e = idx[i];
			// a caption prefix ranks with an id prefix, so typing "Сид" puts
			// Сидорович at the top instead of after every substring hit
			bool prefix = q.empty() || 0 == _strnicmp(e.id.c_str(), q.c_str(), q.size());
			if (!prefix && !e.name.empty())
			{
				xr_string h, n;
				FoldNoCase(e.name, h);
				FoldNoCase(q, n);
				prefix = h.size() >= n.size() && 0 == strncmp(h.c_str(), n.c_str(), n.size());
			}
			if (pass == 0 && !prefix) continue;
			if (pass == 1 && (prefix || !(ContainsNoCase(e.id, q) || ContainsNoCase(e.name, q)))) continue;
			out.push_back(&e);
		}

	// A story id read out of the spawn belongs to an object the game placed; the
	// same NPC also appears as the respawn template it was made from, and that one
	// resolves to nothing at runtime. Both look alike in the list and carry the same
	// caption, so the one that works is offered first.
	if (t == tStory)
		std::stable_partition(out.begin(), out.end(),
			[](const SEntry* e) { return 0 == _strnicmp(e->extra.c_str(), "spawn", 5); });
}

void NqPickers::McpLookup(LPCSTR raw, xr_string& out)
{
	xr_string type, query, path;
	NqUtil::ArgString(raw, "type", type);
	NqUtil::ArgString(raw, "query", query);
	NqUtil::ArgString(raw, "path", path);
	int limit = NqUtil::ArgInt(raw, "limit", 50);
	if (limit <= 0) limit = 50;
	if (type.empty()) { NqUtil::JsonError(out, "missing 'type' (item_section, squad_section, level, smart, story_id, profile, community, info, spot_type, task_id, var_name, ref_name, node_id, quest_id)"); return; }

	xr_vector<SEntry> doc_entries;
	const EType t = TypeFromName(type.c_str());
	bool doc_scoped = false;
	if (t == tCount)
	{
		doc_scoped = true;
		// document sources
		if (type == "quest_id")
		{
			xr_vector<NqDocs::SProjectQuest> all;
			NqDocs::ProjectQuests(all);
			for (u32 i = 0; i < all.size(); ++i)
			{
				SEntry e; e.id = all[i].id; e.name = all[i].title; e.extra = NqUtil::ProjectRelative(all[i].path.c_str());
				doc_entries.push_back(e);
			}
		}
		else if (type == "task_id" || type == "var_name" || type == "ref_name" || type == "node_id")
		{
			if (path.empty()) { NqUtil::JsonError(out, "document-scoped type needs 'path'"); return; }
			SNqQuest q;
			xr_string err;
			if (!NqDocs::ReadQuest(path.c_str(), q, err)) { NqUtil::JsonError(out, err); return; }
			if (type == "task_id")
				for (u32 i = 0; i < q.tasks.size(); ++i) { SEntry e; e.id = q.tasks[i].id; e.name = NqText::Preview(q.tasks[i].title); doc_entries.push_back(e); }
			else if (type == "var_name")
				for (u32 i = 0; i < q.vars.size(); ++i) { SEntry e; e.id = q.vars[i].name; e.name = NqText::ValueSummary(q.vars[i].value); doc_entries.push_back(e); }
			else if (type == "node_id")
				for (u32 i = 0; i < q.nodes.size(); ++i) { SEntry e; e.id = q.nodes[i].id; e.name = q.nodes[i].kind; doc_entries.push_back(e); }
			else
			{
				xr_vector<xr_string> refs;
				NqValidate::DeclaredRefs(q, refs);
				for (u32 i = 0; i < refs.size(); ++i) { SEntry e; e.id = refs[i]; doc_entries.push_back(e); }
			}
		}
		else { NqUtil::JsonError(out, "unknown lookup type"); return; }
	}

	xr_vector<const SEntry*> found;
	if (doc_scoped)
	{
		for (u32 i = 0; i < doc_entries.size() && (int)found.size() < limit; ++i)
			if (query.empty() || ContainsNoCase(doc_entries[i].id, query) || ContainsNoCase(doc_entries[i].name, query))
				found.push_back(&doc_entries[i]);
	}
	else Search(t, query.c_str(), limit, found);

	out = "{\"ok\":true,\"type\":";
	NqUtil::JsonString(out, type);
	out += ",\"available\":"; NqUtil::JsonBool(out, doc_scoped || s_Available);
	out += ",\"total\":"; NqUtil::JsonInt(out, doc_scoped ? (int)doc_entries.size() : (int)Index(t).size());
	out += ",\"entries\":[";
	for (u32 i = 0; i < found.size(); ++i)
	{
		if (i) out += ",";
		out += "{\"id\":"; NqUtil::JsonString(out, found[i]->id);
		if (!found[i]->name.empty())	{ out += ",\"name\":"; NqUtil::JsonString(out, found[i]->name); }
		if (!found[i]->extra.empty())	{ out += ",\"extra\":"; NqUtil::JsonString(out, found[i]->extra); }
		out += "}";
	}
	out += "]}";
}
