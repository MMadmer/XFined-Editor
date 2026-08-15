#include "stdafx.h"
#include "NqCatalog.h"
#include "NqUtil.h"
#include "../EditorGameContent.h"
#include "../EditorProject.h"

namespace
{
	xr_vector<NqCatalog::SKind>	s_Kinds;
	xr_vector<xr_string>		s_Files;
	xr_string					s_Source;
	xr_string					s_Error;
	int							s_Version	= 0;
	int							s_Api		= 0;
	bool						s_Loaded	= false;
	xr_string					s_LoadedFor;	// game root the load was made for (auto-reload on link change)

	xr_string CurrentKey()
	{
		xr_string k = (EditorProject::Active() && EditorProject::GameLinked()) ? EditorProject::GameRoot() : "";
		return k;
	}

	// catalog type names - what tells a parameter definition line apart from a
	// kind attribute that happens to share the name (see ParseCatalogText)
	const char* kTypes[] =
	{
		"string", "int", "float", "bool", "enum", "text", "duration", "npc_ref", "target_ref",
		"place", "spawn_spec", "item_section", "squad_section", "level", "smart", "story_id",
		"profile", "community", "info", "var_name", "task_id", "quest_id", "ref_name",
		"signal_name", "spot_type", "relation", "lua", "cases", "cases_cond", "cases_weight",
		"kill_target", "count_or_all", "value", "cond_list", "node_id",
	};

	bool IsTypeName(const xr_string& s)
	{
		for (int i = 0; i < (int)(sizeof(kTypes) / sizeof(kTypes[0])); ++i)
			if (s == kTypes[i]) return true;
		return false;
	}

	const char* kAttrs[] = { "group", "title", "desc", "use", "params", "pins", "wait", "once", "event", "impl", "since", "alias" };

	bool IsAttr(const xr_string& k)
	{
		for (int i = 0; i < (int)(sizeof(kAttrs) / sizeof(kAttrs[0])); ++i)
			if (k == kAttrs[i]) return true;
		return false;
	}

	bool ToBool(const xr_string& v)
	{
		return v == "true" || v == "1" || v == "on" || v == "yes";
	}

	void ParseParamDef(const xr_string& name, const xr_string& spec, NqCatalog::SParam& p)
	{
		p = NqCatalog::SParam();
		p.name = name;
		xr_vector<xr_string> parts;
		NqUtil::Split(spec.c_str(), ',', parts);
		for (u32 i = 0; i < parts.size(); ++i)
		{
			const xr_string& t = parts[i];
			if (i == 0)					{ p.type = t; continue; }
			if (t == "required")		{ p.required = true; continue; }
			const size_t eq = t.find('=');
			if (eq == xr_string::npos)	continue;
			const xr_string k = NqUtil::Trim(t.substr(0, eq)), v = NqUtil::Trim(t.substr(eq + 1));
			if (k == "default")			{ p.has_default = true; p.def = v; }
			else if (k == "min")		{ p.has_min = true; p.min = atof(v.c_str()); }
			else if (k == "max")		{ p.has_max = true; p.max = atof(v.c_str()); }
			else if (k == "enum")		NqUtil::Split(v.c_str(), '|', p.enums);
		}
	}

	// A hand-rolled ltx reader instead of CInifile: CInifile dies on a duplicate
	// section (modules may redeclare a kind) and folds duplicate keys - and the
	// catalog legitimately has a parameter named like a kind attribute
	// (task.set_text: title; elapsed: since). Reading in order keeps both.
	void ParseCatalogText(const xr_string& text_utf8_or_cp, LPCSTR source, bool convert_cp1251,
						  xr_vector<NqCatalog::SKind>& kinds)
	{
		xr_string sect;
		NqCatalog::SKind* cur = 0;
		xr_vector<xr_string> param_names;
		bool after_params = false;
		xr_vector<xr_string> lines;
		{
			size_t pos = 0;
			while (pos <= text_utf8_or_cp.size())
			{
				size_t eol = text_utf8_or_cp.find('\n', pos);
				if (eol == xr_string::npos) eol = text_utf8_or_cp.size();
				lines.push_back(text_utf8_or_cp.substr(pos, eol - pos));
				pos = eol + 1;
			}
		}
		for (u32 li = 0; li < lines.size(); ++li)
		{
			xr_string line = lines[li];
			// comments: ';' anywhere, '//' anywhere (no quoted values in the catalog)
			size_t c = line.find(';');
			if (c != xr_string::npos) line.erase(c);
			c = line.find("//");
			if (c != xr_string::npos) line.erase(c);
			line = NqUtil::Trim(line);
			if (line.empty()) continue;
			if (line[0] == '[')
			{
				const size_t close = line.find(']');
				if (close == xr_string::npos) continue;
				sect = NqUtil::Trim(line.substr(1, close - 1));
				for (u32 i = 0; i < sect.size(); ++i) sect[i] = (char)tolower((unsigned char)sect[i]);
				cur = 0;
				param_names.clear();
				after_params = false;
				if (sect == "nq.catalog")
				{
					// header handled through key lines below
					continue;
				}
				if (0 != strncmp(sect.c_str(), "nq.", 3)) continue;
				const xr_string id = sect.substr(3);
				// a later file redefining a kind wins (module extension)
				for (u32 i = 0; i < kinds.size(); ++i)
					if (kinds[i].id == id) { kinds.erase(kinds.begin() + i); break; }
				NqCatalog::SKind k;
				k.id = id;
				k.source = source ? source : "";
				kinds.push_back(k);
				cur = &kinds.back();
				continue;
			}
			const size_t eq = line.find('=');
			if (eq == xr_string::npos) continue;
			xr_string key = NqUtil::Trim(line.substr(0, eq));
			xr_string val = NqUtil::Trim(line.substr(eq + 1));
			for (u32 i = 0; i < key.size(); ++i) key[i] = (char)tolower((unsigned char)key[i]);
			if (sect == "nq.catalog")
			{
				if (key == "version")	s_Version = atoi(val.c_str());
				else if (key == "api")	s_Api = atoi(val.c_str());
				continue;
			}
			if (!cur) continue;

			bool in_params = false;
			for (u32 i = 0; i < param_names.size(); ++i) if (param_names[i] == key) in_params = true;
			bool have_def = false;
			for (u32 i = 0; i < cur->params.size(); ++i) if (cur->params[i].name == key) have_def = true;
			xr_string first;
			{
				const size_t comma = val.find(',');
				first = NqUtil::Trim(comma == xr_string::npos ? val : val.substr(0, comma));
			}
			const bool looks_like_type = IsTypeName(first);
			// parameter definition: named in `params`, defined after that line,
			// not yet defined, and its value starts with a type name (or the
			// key is not a kind attribute at all)
			if (in_params && after_params && !have_def && (looks_like_type || !IsAttr(key)))
			{
				NqCatalog::SParam p;
				ParseParamDef(key, val, p);
				// keep the `params` order
				u32 at = 0;
				for (u32 i = 0; i < param_names.size(); ++i)
					if (param_names[i] == key) { at = i; break; }
				u32 ins = 0;
				while (ins < cur->params.size())
				{
					u32 other = 0;
					for (u32 i = 0; i < param_names.size(); ++i)
						if (param_names[i] == cur->params[ins].name) { other = i; break; }
					if (other > at) break;
					++ins;
				}
				cur->params.insert(cur->params.begin() + ins, p);
				continue;
			}
			if (key == "group")			cur->group = val;
			else if (key == "title")	cur->title = convert_cp1251 ? NqUtil::Cp1251ToUtf8(val.c_str()) : val;
			else if (key == "desc")		cur->desc  = convert_cp1251 ? NqUtil::Cp1251ToUtf8(val.c_str()) : val;
			else if (key == "use")
			{
				xr_vector<xr_string> u;
				NqUtil::Split(val.c_str(), ',', u);
				cur->use = 0;
				for (u32 i = 0; i < u.size(); ++i)
				{
					if (u[i] == "trigger")		cur->use |= NqCatalog::useTrigger;
					else if (u[i] == "main")	cur->use |= NqCatalog::useMain;
					else if (u[i] == "extra")	cur->use |= NqCatalog::useExtra;
					else if (u[i] == "cond")	cur->use |= NqCatalog::useCond;
				}
			}
			else if (key == "params")
			{
				NqUtil::Split(val.c_str(), ',', param_names);
				after_params = true;
			}
			else if (key == "pins")
			{
				NqUtil::Split(val.c_str(), ',', cur->pins);
				cur->pins_from_cases = false;
				for (u32 i = 0; i < cur->pins.size(); ++i)
					if (cur->pins[i] == "cases") cur->pins_from_cases = true;
			}
			else if (key == "wait")		cur->wait = ToBool(val);
			else if (key == "once")		cur->once_default = ToBool(val);
			else if (key == "event")	cur->event = ToBool(val);
			else if (key == "impl")		cur->impl = val;
			else if (key == "since")	cur->since = atoi(val.c_str());
			// CSV like every other list attribute here: the runtime splits it too
			else if (key == "alias")	NqUtil::Split(val.c_str(), ',', cur->alias);
		}
		// a param named in `params` but never defined still exists (type string)
		// -- no: an undefined param would hide catalog typos; leave it out.
	}

	bool ReadWholeFile(LPCSTR path, xr_string& text)
	{
		xr_string err;
		HANDLE h = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == h) return false;
		LARGE_INTEGER size;
		if (!::GetFileSizeEx(h, &size) || size.QuadPart > (8 << 20)) { ::CloseHandle(h); return false; }
		xr_vector<char> buf((size_t)size.QuadPart + 1, 0);
		DWORD got = 0;
		if (size.QuadPart) ::ReadFile(h, buf.data(), (DWORD)size.QuadPart, &got, NULL);
		::CloseHandle(h);
		text.assign(buf.data(), buf.data() + got);
		return true;
	}

	void LoadGameItem(int idx, LPCSTR label)
	{
		u32 size = 0;
		u8* bytes = EditorGameContent::ReadBytes(idx, size);
		if (!bytes || !size) { if (bytes) EditorGameContent::FreeBytes(bytes); return; }
		xr_string text; text.assign((const char*)bytes, (const char*)bytes + size);
		EditorGameContent::FreeBytes(bytes);
		ParseCatalogText(text, label, true, s_Kinds);
		s_Files.push_back(xr_string(label));
	}

	void LoadDiskFile(LPCSTR path, LPCSTR label)
	{
		xr_string text;
		if (!ReadWholeFile(path, text)) return;
		ParseCatalogText(text, label, true, s_Kinds);
		s_Files.push_back(xr_string(label));
	}

	// <root>\<sub>\*\gamedata\configs\nq\kinds\*.ltx
	void LoadModuleKinds(LPCSTR game, LPCSTR sub)
	{
		string_path mask;
		sprintf_s(mask, "%s\\%s\\*", game, sub);
		WIN32_FIND_DATAA fd;
		HANDLE h = ::FindFirstFileA(mask, &fd);
		if (INVALID_HANDLE_VALUE == h) return;
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
			if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
			string_path dir;
			sprintf_s(dir, "%s\\%s\\%s\\gamedata\\configs\\nq\\kinds", game, sub, fd.cFileName);
			xr_vector<xr_string> files;
			NqUtil::ListFiles(dir, ".ltx", files);
			for (u32 i = 0; i < files.size(); ++i)
			{
				string_path label;
				sprintf_s(label, "%s\\%s\\kinds\\%s", sub, fd.cFileName, NqUtil::BaseName(files[i].c_str()).c_str());
				LoadDiskFile(files[i].c_str(), label);
			}
		} while (::FindNextFileA(h, &fd));
		::FindClose(h);
	}

	void Load()
	{
		s_Kinds.clear(); s_Files.clear(); s_Source.clear(); s_Error.clear();
		s_Version = 0; s_Api = 0;
		s_Loaded = true;
		s_LoadedFor = CurrentKey();

		bool from_game = false;
		xr_string mount_err;
		if (EditorProject::Active() && EditorProject::GameLinked() && EditorGameContent::EnsureMounted(mount_err))
		{
			const int idx = EditorGameContent::Find("configs\\nq\\catalog.ltx");
			if (idx >= 0)
			{
				from_game = true;
				LoadGameItem(idx, "game:configs\\nq\\catalog.ltx");
				// extensions shipped inside the game content
				const int total = EditorGameContent::Count();
				for (int i = 0; i < total; ++i)
				{
					const EditorGameContent::SItem* it = EditorGameContent::Get(i);
					if (!it || 0 != _strnicmp(it->path.c_str(), "configs\\nq\\kinds\\", 17)) continue;
					if (it->path.size() < 4 || 0 != _stricmp(it->path.c_str() + it->path.size() - 4, ".ltx")) continue;
					string_path label;
					sprintf_s(label, "game:%s", it->path.c_str());
					LoadGameItem(i, label);
				}
			}
		}
		if (!from_game)
		{
			string_path path;
			sprintf_s(path, "%s\\gamedata\\configs\\nq\\catalog.ltx", NqUtil::SdkRoot());
			if (!NqUtil::FileExists(path))
			{
				s_Error = "bundled catalog not found: ";
				s_Error += path;
				return;
			}
			LoadDiskFile(path, "bundled:configs\\nq\\catalog.ltx");
			string_path kinds_dir;
			sprintf_s(kinds_dir, "%s\\gamedata\\configs\\nq\\kinds", NqUtil::SdkRoot());
			xr_vector<xr_string> files;
			NqUtil::ListFiles(kinds_dir, ".ltx", files);
			for (u32 i = 0; i < files.size(); ++i)
			{
				string_path label;
				sprintf_s(label, "bundled:configs\\nq\\kinds\\%s.ltx", NqUtil::BaseName(files[i].c_str()).c_str());
				LoadDiskFile(files[i].c_str(), label);
			}
		}
		// installed modules extend the catalog whichever core catalog is used
		if (EditorProject::Active() && EditorProject::GameLinked())
		{
			LoadModuleKinds(EditorProject::GameRoot(), "modules");
			LoadModuleKinds(EditorProject::GameRoot(), "mods");
		}
		s_Source = from_game ? "game" : "bundled";
		if (s_Kinds.empty()) s_Error = "the catalog declares no kinds";
	}
}

//------------------------------------------------------------------------------
const NqCatalog::SParam* NqCatalog::SKind::Param(LPCSTR name) const
{
	if (!name) return 0;
	for (u32 i = 0; i < params.size(); ++i) if (params[i].name == name) return &params[i];
	return 0;
}

bool NqCatalog::SKind::HasPin(LPCSTR pin) const
{
	if (!pin) return false;
	for (u32 i = 0; i < pins.size(); ++i) if (pins[i] == pin && pins[i] != "cases") return true;
	return false;
}

void NqCatalog::Invalidate()	{ s_Loaded = false; }

bool NqCatalog::Ensure()
{
	// a changed game link means a different catalog; reload without asking
	if (!s_Loaded || s_LoadedFor != CurrentKey()) Load();
	return !s_Kinds.empty();
}

LPCSTR	NqCatalog::Source()		{ Ensure(); return s_Source.c_str(); }
int		NqCatalog::Version()	{ Ensure(); return s_Version; }
int		NqCatalog::Api()		{ Ensure(); return s_Api; }
LPCSTR	NqCatalog::LoadError()	{ Ensure(); return s_Error.c_str(); }
const xr_vector<NqCatalog::SKind>& NqCatalog::Kinds()	{ Ensure(); return s_Kinds; }
const xr_vector<xr_string>& NqCatalog::Files()			{ Ensure(); return s_Files; }

const NqCatalog::SKind* NqCatalog::Find(LPCSTR id)
{
	Ensure();
	if (!id) return 0;
	for (u32 i = 0; i < s_Kinds.size(); ++i) if (s_Kinds[i].id == id) return &s_Kinds[i];
	// aliases: an old name still resolves
	for (u32 i = 0; i < s_Kinds.size(); ++i)
		for (u32 a = 0; a < s_Kinds[i].alias.size(); ++a)
			if (s_Kinds[i].alias[a] == id) return &s_Kinds[i];
	return 0;
}

void NqCatalog::KindsFor(u32 use_mask, xr_vector<const SKind*>& out)
{
	Ensure();
	out.clear();
	for (u32 i = 0; i < s_Kinds.size(); ++i) if (s_Kinds[i].use & use_mask) out.push_back(&s_Kinds[i]);
}

void NqCatalog::McpCatalog(xr_string& out)
{
	if (!Ensure())
	{
		NqUtil::JsonError(out, s_Error.empty() ? "catalog not available" : s_Error.c_str());
		return;
	}
	out = "{\"ok\":true,\"source\":";
	NqUtil::JsonString(out, s_Source);
	out += ",\"version\":"; NqUtil::JsonInt(out, s_Version);
	out += ",\"api\":";	NqUtil::JsonInt(out, s_Api);
	out += ",\"files\":"; NqUtil::JsonStringArray(out, s_Files);
	out += ",\"count\":"; NqUtil::JsonInt(out, (int)s_Kinds.size());
	out += ",\"kinds\":[";
	for (u32 i = 0; i < s_Kinds.size(); ++i)
	{
		const SKind& k = s_Kinds[i];
		if (i) out += ",";
		out += "{\"id\":";		NqUtil::JsonString(out, k.id);
		out += ",\"group\":";	NqUtil::JsonString(out, k.group);
		out += ",\"title\":";	NqUtil::JsonString(out, k.title);
		out += ",\"desc\":";	NqUtil::JsonString(out, k.desc);
		out += ",\"use\":[";
		bool first = true;
		if (k.use & useTrigger)	{ out += "\"trigger\"";	first = false; }
		if (k.use & useMain)	{ if (!first) out += ","; out += "\"main\"";	first = false; }
		if (k.use & useExtra)	{ if (!first) out += ","; out += "\"extra\"";	first = false; }
		if (k.use & useCond)	{ if (!first) out += ","; out += "\"cond\"";	first = false; }
		out += "],\"params\":[";
		for (u32 p = 0; p < k.params.size(); ++p)
		{
			const SParam& pr = k.params[p];
			if (p) out += ",";
			out += "{\"name\":";	NqUtil::JsonString(out, pr.name);
			out += ",\"type\":";	NqUtil::JsonString(out, pr.type);
			out += ",\"required\":"; NqUtil::JsonBool(out, pr.required);
			if (pr.has_default)	{ out += ",\"default\":"; NqUtil::JsonString(out, pr.def); }
			if (pr.has_min)		{ out += ",\"min\":"; NqUtil::JsonNumber(out, pr.min); }
			if (pr.has_max)		{ out += ",\"max\":"; NqUtil::JsonNumber(out, pr.max); }
			if (!pr.enums.empty()) { out += ",\"enum\":"; NqUtil::JsonStringArray(out, pr.enums); }
			out += "}";
		}
		out += "]";
		if (k.use & (useMain | useTrigger))
		{
			out += ",\"pins\":";	NqUtil::JsonStringArray(out, k.pins);
			out += ",\"wait\":";	NqUtil::JsonBool(out, k.wait);
			out += ",\"once\":";	NqUtil::JsonBool(out, k.once_default);
		}
		if (k.use & useCond)	{ out += ",\"event\":"; NqUtil::JsonBool(out, k.event); }
		out += ",\"impl\":";	NqUtil::JsonString(out, k.impl);
		out += ",\"since\":";	NqUtil::JsonInt(out, k.since);
		if (!k.alias.empty())	{ out += ",\"alias\":"; NqUtil::JsonStringArray(out, k.alias); }
		out += ",\"source\":";	NqUtil::JsonString(out, k.source);
		out += "}";
	}
	out += "]}";
}
