#include "stdafx.h"
#include "EditorModScene.h"

//------------------------------------------------------------------------------
// small file/string helpers (WinAPI only, like EditorModManifest)
//------------------------------------------------------------------------------
static bool DirExists(const char* p)
{
	DWORD a = ::GetFileAttributesA(p);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
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

static bool WriteBinFile(const char* path, const void* data, u32 size)
{
	HANDLE h = ::CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD wr = 0;
	const bool ok = !!::WriteFile(h, data, size, &wr, NULL) && wr == size;
	::CloseHandle(h);
	return ok;
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

//------------------------------------------------------------------------------
// json / argument helpers
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

// bare (unquoted) JSON values: {"selected_only":false} / {"sector":3}
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

static int ArgInt(LPCSTR raw, LPCSTR field, int def)
{
	char pat[64];
	sprintf_s(pat, "\"%s\"", field);
	const char* k = raw ? strstr(raw, pat) : 0;
	if (!k) return def;
	const char* c = strchr(k + xr_strlen(pat), ':');
	if (!c) return def;
	return atoi(c + 1);
}

//------------------------------------------------------------------------------
// level / name resolution
//------------------------------------------------------------------------------
// explicit `level` argument wins; otherwise the opened scene file name
// (path and extension stripped, e.g. "...\l01_escape.level" -> "l01_escape")
static bool ResolveLevelName(LPCSTR arg, char* dst, u32 size)
{
	if (arg && arg[0])
	{
		strncpy_s(dst, size, arg, _TRUNCATE);
		return true;
	}
	LPCSTR fn = Tools ? Tools->m_LastFileName.c_str() : "";
	if (!fn || !fn[0]) return false;
	const char* base = fn;
	for (const char* p = fn; *p; ++p)
		if (*p == '\\' || *p == '/') base = p + 1;
	strncpy_s(dst, size, base, _TRUNCATE);
	if (char* dot = strrchr(dst, '.')) *dot = 0;
	return 0 != dst[0];
}

// object names go into file names - anything unsafe becomes '_'
static void SanitizeFileName(LPCSTR src, char* dst, u32 size)
{
	u32 o = 0;
	for (const char* p = src ? src : ""; *p && o + 1 < size; ++p)
	{
		const char c = *p;
		const bool bad = c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
						 c == '"' || c == '<' || c == '>' || c == '|' || c == ' ';
		dst[o++] = bad ? '_' : c;
	}
	dst[o] = 0;
	if (!o) strncpy_s(dst, size, "unnamed", _TRUNCATE);
}

//------------------------------------------------------------------------------
// collision overlay (.xcform)
//------------------------------------------------------------------------------
struct SXCTri
{
	u32		v[3];
	u16		mtl;
};

static int MtlIndex(xr_vector<xr_string>& mtls, LPCSTR name)
{
	for (u32 i = 0; i < mtls.size(); ++i)
		if (mtls[i] == name) return (int)i;
	mtls.push_back(xr_string(name));
	return (int)mtls.size() - 1;
}

// XCF1 v2 keeps a cut list after the triangles: boxes that delete BASE level
// triangles at load. Both exports go through the same file, so each one has to
// preserve what the other wrote - hence the read-back.
struct SXCForm
{
	xr_vector<Fvector>		verts;
	xr_vector<SXCTri>		tris;
	xr_vector<xr_string>	mtls;
	xr_vector<Fbox>			cuts;
	u16						sector;
	SXCForm() : sector(0) {}
};

static bool ReadXCForm(const char* path, SXCForm& out)
{
	out = SXCForm();
	xr_string raw;
	if (!ReadTextFile(path, raw) || raw.size() < 16) return false;

	const u8* cur = (const u8*)raw.c_str();
	const u8* end = cur + raw.size();
	const auto u32at = [&]() { u32 v; memcpy(&v, cur, 4); cur += 4; return v; };
	const auto u16at = [&]() { u16 v; memcpy(&v, cur, 2); cur += 2; return v; };
	const auto f32at = [&]() { float v; memcpy(&v, cur, 4); cur += 4; return v; };

	if (u32at() != 0x31464358) return false;
	const u32 version = u32at();
	if (version != 1 && version != 2) return false;
	out.sector = u16at();
	u16at();										// reserved
	const u32 mtl_count = u32at();
	for (u32 i = 0; i < mtl_count && cur < end; ++i)
	{
		const char* name = (const char*)cur;
		cur += xr_strlen(name) + 1;
		out.mtls.push_back(xr_string(name));
	}
	if (cur + 4 > end) return false;
	const u32 vcount = u32at();
	if (cur + size_t(vcount) * 12 + 4 > end) return false;
	for (u32 i = 0; i < vcount; ++i)
	{
		Fvector v; v.x = f32at(); v.y = f32at(); v.z = f32at();
		out.verts.push_back(v);
	}
	const u32 tcount = u32at();
	if (cur + size_t(tcount) * sizeof(SXCTri) + size_t(tcount) * 2 > end) return false;
	for (u32 i = 0; i < tcount; ++i)
	{
		SXCTri t;
		t.v[0] = u32at(); t.v[1] = u32at(); t.v[2] = u32at();
		t.mtl = u16at();
		u16at();									// pad
		out.tris.push_back(t);
	}
	if (version >= 2 && cur + 4 <= end)
	{
		const u32 ccount = u32at();
		for (u32 i = 0; i < ccount && cur + 24 <= end; ++i)
		{
			Fvector c, e;
			c.x = f32at(); c.y = f32at(); c.z = f32at();
			e.x = f32at(); e.y = f32at(); e.z = f32at();
			Fbox b; b.min.sub(c, e); b.max.add(c, e);
			out.cuts.push_back(b);
		}
	}
	return true;
}

static bool WriteXCForm(const char* path, const SXCForm& in)
{
	CMemoryWriter F;
	F.w_u32(0x31464358);							// "XCF1"
	F.w_u32(2);										// version: cut list present
	F.w_u16(in.sector);
	F.w_u16(0);										// reserved
	F.w_u32((u32)in.mtls.size());
	for (u32 i = 0; i < in.mtls.size(); ++i)
		F.w_stringZ(in.mtls[i].c_str());
	F.w_u32((u32)in.verts.size());
	for (u32 i = 0; i < in.verts.size(); ++i)
	{
		F.w_float(in.verts[i].x);
		F.w_float(in.verts[i].y);
		F.w_float(in.verts[i].z);
	}
	F.w_u32((u32)in.tris.size());
	for (u32 i = 0; i < in.tris.size(); ++i)
	{
		F.w_u32(in.tris[i].v[0]);
		F.w_u32(in.tris[i].v[1]);
		F.w_u32(in.tris[i].v[2]);
		F.w_u16(in.tris[i].mtl);
		F.w_u16(0);
	}
	F.w_u32((u32)in.cuts.size());
	for (u32 i = 0; i < in.cuts.size(); ++i)
	{
		Fvector c, e;
		in.cuts[i].getcenter(c);
		e.sub(in.cuts[i].max, c);					// half-extents, matches Fbox::setb
		F.w_float(c.x); F.w_float(c.y); F.w_float(c.z);
		F.w_float(e.x); F.w_float(e.y); F.w_float(e.z);
	}
	return WriteBinFile(path, F.pointer(), F.size());
}

static bool DoExportXCForm(LPCSTR level_arg, int sector, bool selected_only,
						   xr_string& out_path, int& o_verts, int& o_tris, int& o_mtls, xr_string& err)
{
	out_path.clear(); o_verts = o_tris = o_mtls = 0; err.clear();
	if (!EditorProject::Active())		{ err = "no active project"; return false; }
	if (!Scene)							{ err = "no scene"; return false; }
	if (sector < 0 || sector > 65535)	{ err = "sector out of u16 range"; return false; }
	char level[128];
	if (!ResolveLevelName(level_arg, level, sizeof(level)))
										{ err = "no level name - open a scene or pass 'level'"; return false; }

	xr_vector<Fvector>		verts;
	xr_vector<SXCTri>		tris;
	xr_vector<xr_string>	mtls;
	int objects_used = 0;

	ObjectList& lst = Scene->ListObj(OBJCLASS_SCENEOBJECT);
	for (ObjectIt it = lst.begin(); it != lst.end(); ++it)
	{
		if (selected_only && !(*it)->Selected()) continue;
		CSceneObject* so = dynamic_cast<CSceneObject*>(*it);
		if (!so) continue;
		CEditableObject* eo = so->GetReference();
		if (!eo) continue;
		const Fmatrix& xf = so->_Transform();
		bool used = false;
		for (EditMeshIt m = eo->FirstMesh(); m != eo->LastMesh(); ++m)
		{
			CEditableMesh* mesh = *m;
			const u32 vc = mesh->GetVCount();
			const u32 fc = mesh->GetFCount();
			if (!vc || !fc) continue;

			const u32 base = (u32)verts.size();
			const Fvector* mv = mesh->GetVertices();
			for (u32 i = 0; i < vc; ++i)
			{
				Fvector w;
				xf.transform_tiny(w, mv[i]);
				verts.push_back(w);
			}

			// per-face material via the surface->faces map; uncovered faces
			// (broken surface data) fall back to the default game material
			xr_vector<int> fmtl(fc, -1);
			SurfFaces& sf = mesh->Surfaces();
			for (SurfFacesPairIt sp = sf.begin(); sp != sf.end(); ++sp)
			{
				LPCSTR gm = sp->first ? sp->first->_GameMtlName() : 0;
				const int mi = MtlIndex(mtls, (gm && gm[0]) ? gm : "materials\\default");
				IntVec& ids = sp->second;
				for (u32 i = 0; i < ids.size(); ++i)
					if ((u32)ids[i] < fc) fmtl[ids[i]] = mi;
			}

			const st_Face* mf = mesh->GetFaces();
			for (u32 f = 0; f < fc; ++f)
			{
				SXCTri t;
				t.v[0] = base + (u32)mf[f].pv[0].pindex;
				t.v[1] = base + (u32)mf[f].pv[1].pindex;
				t.v[2] = base + (u32)mf[f].pv[2].pindex;
				t.mtl = (u16)((fmtl[f] >= 0) ? fmtl[f] : MtlIndex(mtls, "materials\\default"));
				tris.push_back(t);
			}
			used = true;
		}
		if (used) ++objects_used;
	}

	if (tris.empty())
	{
		err = selected_only ? "no triangles collected - select scene objects first"
							: "no triangles collected - the scene has no static objects";
		return false;
	}
	if (mtls.size() > 65535)	{ err = "too many materials"; return false; }

	string_path dir, path;
	sprintf_s(dir, "%s\\levels\\%s", EditorProject::Root(), level);
	CreateDirChain(dir);
	if (!DirExists(dir))		{ err = "can't create the overlay folder"; return false; }
	sprintf_s(path, "%s\\overlay.xcform", dir);

	// keep whatever cuts the cut export left there - only the geometry is ours
	SXCForm file;
	ReadXCForm(path, file);
	file.sector	= (u16)sector;
	file.verts	= verts;
	file.tris	= tris;
	file.mtls	= mtls;
	if (!WriteXCForm(path, file))
								{ err = "can't write overlay.xcform"; return false; }

	out_path = path;
	o_verts = (int)verts.size();
	o_tris = (int)tris.size();
	o_mtls = (int)mtls.size();
	Msg("* [XMS] collision overlay: %d object(s), %d tri(s) -> %s", objects_used, o_tris, path);
	return true;
}

//------------------------------------------------------------------------------
// world-space visual overlay (.ogf) - reuses the SDK CExportObjectOGF pipeline
// through CEditableObject::ExportOGF on a private world-transformed copy
//------------------------------------------------------------------------------
struct SOGFResult
{
	int			exported;
	int			skipped_dynamic;	// skeleton/dynamic refs: not world-bakeable
	int			skipped_failed;		// missing .object source or export failure
	xr_string	dir;
	xr_string	names_json;			// ready ["a","b"] payload
	SOGFResult() : exported(0), skipped_dynamic(0), skipped_failed(0) {}
};

static bool DoExportOGF(LPCSTR level_arg, bool selected_only, SOGFResult& res, xr_string& err)
{
	res = SOGFResult(); err.clear();
	if (!EditorProject::Active())	{ err = "no active project"; return false; }
	if (!Scene)						{ err = "no scene"; return false; }
	char level[128];
	if (!ResolveLevelName(level_arg, level, sizeof(level)))
									{ err = "no level name - open a scene or pass 'level'"; return false; }

	string_path dir, ltx;
	sprintf_s(dir, "%s\\levels\\%s\\visuals", EditorProject::Root(), level);
	sprintf_s(ltx, "%s\\levels\\%s\\overlay_visuals.ltx", EditorProject::Root(), level);

	// existing registry: reused for entry numbering and per-file dedup
	xr_string ltx_text;
	ReadTextFile(ltx, ltx_text);
	int next_index = 0;
	for (size_t p = ltx_text.find("[v_"); p != xr_string::npos; p = ltx_text.find("[v_", p + 3))
		++next_index;

	res.names_json = "[";
	xr_vector<xr_string> used_names;
	ObjectList& lst = Scene->ListObj(OBJCLASS_SCENEOBJECT);
	for (ObjectIt it = lst.begin(); it != lst.end(); ++it)
	{
		if (selected_only && !(*it)->Selected()) continue;
		CSceneObject* so = dynamic_cast<CSceneObject*>(*it);
		if (!so) continue;
		CEditableObject* ref = so->GetReference();
		if (!ref) continue;
		// TranslateToWorld only bakes vertices - skeletons keep local bones
		if (ref->IsSkeleton() || ref->IsDynamic()) { ++res.skipped_dynamic; continue; }
		LPCSTR ref_name = so->RefName();
		if (!ref_name || !ref_name[0])	{ ++res.skipped_failed; continue; }

		string_path src;
		FS.update_path(src, _objects_, EFS.ChangeFileExt(ref_name, ".object").c_str());
		if (!FS.exist(src))				{ ++res.skipped_failed; continue; }

		// Two scene names can sanitise to the same file name; without a suffix the
		// second export would overwrite the first .ogf and the registry would keep
		// a single entry, so one object would silently disappear from the level.
		char safe[256];
		SanitizeFileName(so->GetName(), safe, sizeof(safe));
		for (u32 dup = 2; ; ++dup)
		{
			bool taken = false;
			for (u32 i = 0; i < used_names.size(); ++i)
				if (0 == xr_strcmp(used_names[i].c_str(), safe)) { taken = true; break; }
			if (!taken) break;
			char base[256];
			SanitizeFileName(so->GetName(), base, sizeof(base));
			sprintf_s(safe, "%.*s_%u", int(sizeof(safe) - 8), base, dup);
		}
		used_names.push_back(xr_string(safe));
		CreateDirChain(dir);
		string_path dst;
		sprintf_s(dst, "%s\\%s.ogf", dir, safe);

		// private copy: the shared library instance must never be world-transformed
		CEditableObject* tmp = xr_new<CEditableObject>(safe);
		bool ok = tmp->Load(src);
		if (ok)
		{
			tmp->TranslateToWorld(so->_Transform());
			ok = tmp->ExportOGF(dst, 4);
		}
		xr_delete(tmp);
		if (!ok)						{ ++res.skipped_failed; continue; }

		char fileline[512];
		sprintf_s(fileline, "file = visuals/%s.ogf", safe);
		if (ltx_text.find(fileline) == xr_string::npos)
		{
			if (!ltx_text.empty() && ltx_text[ltx_text.size() - 1] != '\n')
				ltx_text += "\r\n";
			char sect[600];
			sprintf_s(sect, "[v_%d]\r\n%s\r\n\r\n", next_index++, fileline);
			ltx_text += sect;
		}

		if (res.exported) res.names_json += ",";
		res.names_json += "\"";
		JsonAppend(res.names_json, safe);
		res.names_json += "\"";
		++res.exported;
	}
	res.names_json += "]";
	res.dir = dir;

	if (!res.exported)
	{
		if (res.skipped_dynamic || res.skipped_failed)
			err = "nothing exported - only skeleton/dynamic or unresolvable objects in the selection";
		else
			err = selected_only ? "no scene objects selected" : "the scene has no static objects";
		return false;
	}

	if (!WriteBinFile(ltx, ltx_text.c_str(), (u32)ltx_text.size()))
									{ err = "visuals exported but can't write overlay_visuals.ltx"; return false; }
	Msg("* [XMS] visual overlay: %d ogf(s) -> %s", res.exported, dir);
	return true;
}

//------------------------------------------------------------------------------
// additive spawn layer (.xspawn)
//
// The delta way to put a model on a level: instead of shipping a rewritten
// level, the module ships a spawn op and the engine composes it into the spawn
// registry at load. Two modules dropping different props on the same map are
// independent ops and never touch each other; the base all.spawn is untouched,
// so saves stay compatible.
//------------------------------------------------------------------------------
struct SSpawnResult
{
	int			exported;
	int			skipped;		// no reference / unusable object
	int			scaled;			// non-unit scale: the spawn path has no scale
	xr_string	path;
	xr_string	names_json;
	SSpawnResult() : exported(0), skipped(0), scaled(0) {}
};

// section names double as file-safe ids; keep the xspawn key charset tight
static void SanitizeOpName(LPCSTR src, char* dst, u32 size)
{
	u32 o = 0;
	for (const char* p = src ? src : ""; *p && o + 1 < size; ++p)
	{
		const char c = *p;
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
						(c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
		dst[o++] = ok ? c : '_';
	}
	dst[o] = 0;
	if (!o) strncpy_s(dst, size, "unnamed", _TRUNCATE);
}

// Rewrites the entries we are exporting now and keeps every other section
// verbatim, so hand-written ops in the same file survive a re-export.
static void MergeSpawnText(const xr_string& old_text, const xr_vector<xr_string>& replaced,
						   const xr_string& fresh, xr_string& out)
{
	out.clear();
	bool dropping = false;
	size_t pos = 0;
	while (pos <= old_text.size())
	{
		size_t eol = old_text.find('\n', pos);
		if (eol == xr_string::npos) eol = old_text.size();
		xr_string line = old_text.substr(pos, eol - pos);

		xr_string trimmed = line;
		while (!trimmed.empty() && (trimmed[0] == ' ' || trimmed[0] == '\t'))
			trimmed.erase(0, 1);

		if (!trimmed.empty() && trimmed[0] == '[')
		{
			const size_t close = trimmed.find(']');
			xr_string header = (close != xr_string::npos) ? trimmed.substr(1, close - 1) : xr_string();
			dropping = false;
			for (u32 i = 0; i < replaced.size(); ++i)
				if (replaced[i] == header) { dropping = true; break; }
		}
		if (!dropping)
		{
			out += line;
			if (eol < old_text.size()) out += '\n';
		}
		if (eol == old_text.size()) break;
		pos = eol + 1;
	}

	while (!out.empty() && (out[out.size() - 1] == '\n' || out[out.size() - 1] == '\r'))
		out.erase(out.size() - 1, 1);
	if (!out.empty()) out += "\r\n\r\n";
	out += fresh;
}

static bool DoExportSpawn(LPCSTR level_arg, bool selected_only, LPCSTR section_arg, LPCSTR mode_arg,
						  SSpawnResult& res, xr_string& err)
{
	res = SSpawnResult(); err.clear();
	if (!EditorProject::Active())	{ err = "no active project"; return false; }
	if (!Scene)						{ err = "no scene"; return false; }
	char level[128];
	if (!ResolveLevelName(level_arg, level, sizeof(level)))
									{ err = "no level name - open a scene or pass 'level'"; return false; }

	// O_PHYS_S with an overridden visual is the vanilla recipe for a placed prop
	const char* section = (section_arg && section_arg[0]) ? section_arg : "physic_object";

	xr_string fresh;
	xr_vector<xr_string> replaced;
	res.names_json = "[";

	ObjectList& lst = Scene->ListObj(OBJCLASS_SCENEOBJECT);
	for (ObjectIt it = lst.begin(); it != lst.end(); ++it)
	{
		if (selected_only && !(*it)->Selected()) continue;
		CSceneObject* so = dynamic_cast<CSceneObject*>(*it);
		if (!so)						{ ++res.skipped; continue; }
		LPCSTR ref_name = so->RefName();
		if (!ref_name || !ref_name[0])	{ ++res.skipped; continue; }

		// Sanitising collapses different scene names onto one key ("box 1" and
		// "box-1" both become "box_1"); without this the later op would silently
		// replace the earlier one and an object would just not spawn.
		char op_name[128];
		SanitizeOpName(so->GetName(), op_name, sizeof(op_name));
		for (u32 dup = 2; ; ++dup)
		{
			bool taken = false;
			for (u32 i = 0; i < replaced.size(); ++i)
				if (0 == xr_strcmp(replaced[i].c_str() + 4 /*"obj:"*/, op_name)) { taken = true; break; }
			if (!taken) break;
			char base[128];
			SanitizeOpName(so->GetName(), base, sizeof(base));
			sprintf_s(op_name, "%.*s_%u", int(sizeof(op_name) - 8), base, dup);
		}

		// visual paths in configs carry no extension - the engine appends .ogf
		char visual[256];
		strncpy_s(visual, ref_name, _TRUNCATE);
		if (char* dot = strrchr(visual, '.')) *dot = 0;

		const Fmatrix& xf = so->_Transform();
		Fvector angles;
		xf.getXYZ(angles);				// mirror of the engine's XFORM().setXYZ(o_Angle)

		const Fvector& scale = so->GetScale();
		if (!fsimilar(scale.x, 1.f) || !fsimilar(scale.y, 1.f) || !fsimilar(scale.z, 1.f))
			++res.scaled;

		char entry[1024];
		sprintf_s(entry,
			"[obj:%s]\r\n"
			"op        = add\r\n"
			"section   = %s\r\n"
			"level     = %s\r\n"
			"position  = %f,%f,%f\r\n"
			"direction = %f,%f,%f\r\n"
			"visual    = %s\r\n",
			op_name, section, level,
			xf.c.x, xf.c.y, xf.c.z,
			angles.x, angles.y, angles.z,
			visual);
		fresh += entry;
		if (mode_arg && mode_arg[0])
		{
			fresh += "mode      = ";
			fresh += mode_arg;
			fresh += "\r\n";
		}
		fresh += "\r\n";

		char header[160];
		sprintf_s(header, "obj:%s", op_name);
		replaced.push_back(xr_string(header));

		if (res.exported) res.names_json += ",";
		res.names_json += "\"";
		JsonAppend(res.names_json, op_name);
		res.names_json += "\"";
		++res.exported;
	}
	res.names_json += "]";

	if (!res.exported)
	{
		err = selected_only ? "no scene objects selected" : "the scene has no placeable objects";
		return false;
	}

	string_path dir, path;
	sprintf_s(dir, "%s\\spawn", EditorProject::Root());
	CreateDirChain(dir);
	if (!DirExists(dir))			{ err = "can't create the spawn folder"; return false; }
	sprintf_s(path, "%s\\%s.xspawn", dir, level);

	xr_string old_text, merged;
	ReadTextFile(path, old_text);
	MergeSpawnText(old_text, replaced, fresh, merged);
	if (!WriteBinFile(path, merged.c_str(), (u32)merged.size()))
									{ err = "can't write the .xspawn file"; return false; }

	res.path = path;
	Msg("* [XMS] spawn layer: %d op(s) -> %s", res.exported, path);
	return true;
}

//------------------------------------------------------------------------------
// level cut (subtractive delta)
//
// The selected objects act as volumes, not as geometry: their world bounds
// become cut boxes that delete base collision triangles and detach base visuals
// at load. Put your own geometry in afterwards (spawn ops / xcform / ogf) and
// you have replaced a chunk of the level without rewriting a single base file.
//------------------------------------------------------------------------------
static bool DoExportCut(LPCSTR level_arg, bool selected_only, bool overlap, float grow,
						int& o_cuts, xr_string& out_path, xr_string& err)
{
	o_cuts = 0; out_path.clear(); err.clear();
	if (!EditorProject::Active())	{ err = "no active project"; return false; }
	if (!Scene)						{ err = "no scene"; return false; }
	char level[128];
	if (!ResolveLevelName(level_arg, level, sizeof(level)))
									{ err = "no level name - open a scene or pass 'level'"; return false; }

	xr_vector<Fbox> boxes;
	ObjectList& lst = Scene->ListObj(OBJCLASS_SCENEOBJECT);
	for (ObjectIt it = lst.begin(); it != lst.end(); ++it)
	{
		if (selected_only && !(*it)->Selected()) continue;
		Fbox bb;
		if (!(*it)->GetBox(bb)) continue;
		if (grow > 0.f) bb.grow(grow);
		boxes.push_back(bb);
	}
	if (boxes.empty())
	{
		err = selected_only ? "no objects selected - select the volumes to cut with"
							: "the scene has no objects to take bounds from";
		return false;
	}

	string_path dir, xcform, ltx;
	sprintf_s(dir, "%s\\levels\\%s", EditorProject::Root(), level);
	CreateDirChain(dir);
	if (!DirExists(dir))			{ err = "can't create the overlay folder"; return false; }
	sprintf_s(xcform, "%s\\overlay.xcform", dir);
	sprintf_s(ltx, "%s\\overlay_visuals.ltx", dir);

	// collision side: replace the cut list, keep the overlay geometry
	SXCForm file;
	ReadXCForm(xcform, file);
	file.cuts = boxes;
	if (!WriteXCForm(xcform, file))	{ err = "can't write overlay.xcform"; return false; }

	// render side: the same boxes as hide entries, rewritten as a block so a
	// re-export does not pile duplicates up
	xr_string old_text, kept;
	ReadTextFile(ltx, old_text);
	{
		bool dropping = false;
		size_t pos = 0;
		while (pos <= old_text.size())
		{
			size_t eol = old_text.find('\n', pos);
			if (eol == xr_string::npos) eol = old_text.size();
			xr_string line = old_text.substr(pos, eol - pos);
			if (!line.empty() && (line[0] == '[' || (line[0] == ' ' && line.find('[') == 1)))
				dropping = line.find("[cut_") != xr_string::npos;
			if (!dropping)
			{
				kept += line;
				if (eol < old_text.size()) kept += '\n';
			}
			if (eol == old_text.size()) break;
			pos = eol + 1;
		}
		while (!kept.empty() && (kept[kept.size() - 1] == '\n' || kept[kept.size() - 1] == '\r'))
			kept.erase(kept.size() - 1, 1);
		if (!kept.empty()) kept += "\r\n\r\n";
	}
	for (u32 i = 0; i < boxes.size(); ++i)
	{
		Fvector c, e;
		boxes[i].getcenter(c);
		e.sub(boxes[i].max, c);
		char entry[512];
		sprintf_s(entry, "[cut_%d]\r\nhide    = %f,%f,%f,%f,%f,%f\r\n%s\r\n",
			i, c.x, c.y, c.z, e.x, e.y, e.z,
			overlap ? "overlap = true" : "; overlap = true  ; uncomment to also take visuals that only touch the box");
		kept += entry;
	}
	if (!WriteBinFile(ltx, kept.c_str(), (u32)kept.size()))
									{ err = "cuts written, but overlay_visuals.ltx is not writable"; return false; }

	o_cuts = (int)boxes.size();
	out_path = xcform;
	Msg("* [XMS] level cut: %d volume(s) -> %s (+ hide entries in overlay_visuals.ltx)", o_cuts, xcform);
	return true;
}

//------------------------------------------------------------------------------
// UI: File\Mod menu triggers + result modal
//------------------------------------------------------------------------------
static const char*	kTitleResult	= "XFined Editor - Overlay Export";
static bool			s_WantMessage	= false;
static char			s_Message[2048]	= {};

void EditorModScene::RequestExportSpawn()
{
	SSpawnResult res;
	xr_string err;
	if (DoExportSpawn(0, true, 0, 0, res, err))
		sprintf_s(s_Message,
			"Spawn layer exported: %d op(s)\n%s\n\nskipped: %d\nobjects with a non-unit scale: %d "
			"(the spawn path carries no scale)",
			res.exported, res.path.c_str(), res.skipped, res.scaled);
	else
		sprintf_s(s_Message, "Spawn layer export failed:\n%s", err.c_str());
	s_WantMessage = true;
}

void EditorModScene::RequestExportCut()
{
	xr_string path, err;
	int cuts = 0;
	if (DoExportCut(0, true, false, 0.f, cuts, path, err))
		sprintf_s(s_Message,
			"Level cut exported: %d volume(s)\n%s\n\nBase collision inside the boxes is removed and base\n"
			"visuals fully inside them are detached. Put your own\ngeometry in with the other exports.",
			cuts, path.c_str());
	else
		sprintf_s(s_Message, "Level cut export failed:\n%s", err.c_str());
	s_WantMessage = true;
}

void EditorModScene::RequestExportXCForm()
{
	xr_string path, err;
	int v = 0, t = 0, m = 0;
	if (DoExportXCForm(0, 0, true, path, v, t, m, err))
		sprintf_s(s_Message,
			"Collision overlay exported:\n%s\n\nvertices: %d\ntriangles: %d\nmaterials: %d",
			path.c_str(), v, t, m);
	else
		sprintf_s(s_Message, "Collision overlay export failed:\n%s", err.c_str());
	s_WantMessage = true;
}

void EditorModScene::RequestExportOGF()
{
	SOGFResult res;
	xr_string err;
	if (DoExportOGF(0, true, res, err))
		sprintf_s(s_Message,
			"Visual overlay exported: %d ogf file(s)\n%s\n\nskipped dynamic: %d, failed: %d\nregistry: overlay_visuals.ltx",
			res.exported, res.dir.c_str(), res.skipped_dynamic, res.skipped_failed);
	else
		sprintf_s(s_Message, "Visual overlay export failed:\n%s", err.c_str());
	s_WantMessage = true;
}

void EditorModScene::DrawUI()
{
	if (!s_WantMessage) return;
	if (!ImGui::BeginPopupModal(kTitleResult, &s_WantMessage, ImGuiWindowFlags_AlwaysAutoResize, true))
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

//------------------------------------------------------------------------------
// MCP commands
//------------------------------------------------------------------------------
void EditorModScene::McpExportSpawn(LPCSTR raw, xr_string& out)
{
	char level[128] = {}, section[128] = {}, mode[128] = {};
	XFinedMCP::GetArg(raw, "level", level, sizeof(level));
	XFinedMCP::GetArg(raw, "section", section, sizeof(section));
	XFinedMCP::GetArg(raw, "mode", mode, sizeof(mode));
	const bool selected_only = ArgBool(raw, "selected_only", true);

	SSpawnResult res;
	xr_string err;
	if (!DoExportSpawn(level[0] ? level : 0, selected_only, section[0] ? section : 0,
					   mode[0] ? mode : 0, res, err))
	{
		out = "{\"ok\":false,\"error\":\"";
		JsonAppend(out, err.c_str());
		out += "\"}";
		return;
	}
	char head[160];
	sprintf_s(head, "{\"ok\":true,\"exported\":%d,\"skipped\":%d,\"scaled\":%d,\"path\":\"",
		res.exported, res.skipped, res.scaled);
	out = head;
	JsonAppendPath(out, res.path.c_str());
	out += "\",\"names\":";
	out += res.names_json;
	out += "}";
}

void EditorModScene::McpExportCut(LPCSTR raw, xr_string& out)
{
	char level[128] = {};
	XFinedMCP::GetArg(raw, "level", level, sizeof(level));
	const bool selected_only	= ArgBool(raw, "selected_only", true);
	const bool overlap			= ArgBool(raw, "overlap", false);
	const float grow			= (float)ArgInt(raw, "grow", 0) / 100.f;	// centimetres

	xr_string path, err;
	int cuts = 0;
	if (!DoExportCut(level[0] ? level : 0, selected_only, overlap, grow, cuts, path, err))
	{
		out = "{\"ok\":false,\"error\":\"";
		JsonAppend(out, err.c_str());
		out += "\"}";
		return;
	}
	char head[96];
	sprintf_s(head, "{\"ok\":true,\"cuts\":%d,\"overlap\":%s,\"path\":\"", cuts, overlap ? "true" : "false");
	out = head;
	JsonAppendPath(out, path.c_str());
	out += "\"}";
}

void EditorModScene::McpExportXCForm(LPCSTR raw, xr_string& out)
{
	char level[128] = {};
	XFinedMCP::GetArg(raw, "level", level, sizeof(level));
	const int sector = ArgInt(raw, "sector", 0);
	const bool selected_only = ArgBool(raw, "selected_only", true);

	xr_string path, err;
	int v = 0, t = 0, m = 0;
	if (!DoExportXCForm(level[0] ? level : 0, sector, selected_only, path, v, t, m, err))
	{
		out = "{\"ok\":false,\"error\":\"";
		JsonAppend(out, err.c_str());
		out += "\"}";
		return;
	}
	out = "{\"ok\":true,\"path\":\"";
	JsonAppendPath(out, path.c_str());
	char tail[128];
	sprintf_s(tail, "\",\"vertices\":%d,\"tris\":%d,\"materials\":%d}", v, t, m);
	out += tail;
}

void EditorModScene::McpExportOGF(LPCSTR raw, xr_string& out)
{
	char level[128] = {};
	XFinedMCP::GetArg(raw, "level", level, sizeof(level));
	const bool selected_only = ArgBool(raw, "selected_only", true);

	SOGFResult res;
	xr_string err;
	if (!DoExportOGF(level[0] ? level : 0, selected_only, res, err))
	{
		out = "{\"ok\":false,\"error\":\"";
		JsonAppend(out, err.c_str());
		out += "\"}";
		return;
	}
	char head[160];
	sprintf_s(head, "{\"ok\":true,\"exported\":%d,\"skipped_dynamic\":%d,\"skipped_failed\":%d,\"dir\":\"",
		res.exported, res.skipped_dynamic, res.skipped_failed);
	out = head;
	JsonAppendPath(out, res.dir.c_str());
	out += "\",\"ltx\":\"overlay_visuals.ltx\",\"names\":";
	out += res.names_json;
	out += "}";
}
