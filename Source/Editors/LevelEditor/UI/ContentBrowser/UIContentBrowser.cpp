#include "stdafx.h"
#include "..\..\..\XrECore\Editor\EditorGameContent.h"
#include "..\..\..\XrECore\Editor\EThumbnailVisual.h"
#include "..\..\..\XrECore\Editor\EDX11Utils.h"
#include "..\..\..\XrECore\Editor\EditorFileOps.h"

// image formats the thumbnail path can decode directly
static bool IsImageExt(LPCSTR name)
{
	LPCSTR ext = name ? strrchr(name, '.') : 0;
	return ext && (!_stricmp(ext, ".dds") || !_stricmp(ext, ".tga") || !_stricmp(ext, ".png") ||
				   !_stricmp(ext, ".jpg") || !_stricmp(ext, ".bmp"));
}

// models carry no baked thumbnail, so their preview has to be rendered
static bool IsVisualExt(LPCSTR name)
{
	LPCSTR ext = name ? strrchr(name, '.') : 0;
	return ext && !_stricmp(ext, ".ogf");
}

// "meshes\dynamics\box\box_1a.ogf" -> "dynamics\box\box_1a", the form
// ::Render->model_Create expects (relative to $game_meshes$, no extension)
static void ToVisualName(LPCSTR path, char* dst, u32 dst_size)
{
	LPCSTR src = path ? path : "";
	if (0 == _strnicmp(src, "meshes\\", 7))	src += 7;
	else if (0 == _strnicmp(src, "meshes/", 7)) src += 7;
	strncpy_s(dst, dst_size, src, _TRUNCATE);
	if (char* dot = strrchr(dst, '.')) *dot = 0;
	for (char* p = dst; *p; ++p) if (*p == '/') *p = '\\';
}

UIContentBrowser* UIContentBrowser::Form = nullptr;

// filled in by the preview window when it is compiled into the build
static UIContentBrowser::TShowVisual	s_ShowVisual	= 0;
static UIContentBrowser::TShowVisualMem	s_ShowVisualMem	= 0;

void UIContentBrowser::SetVisualPreview(TShowVisual by_name, TShowVisualMem from_memory)
{
	s_ShowVisual	= by_name;
	s_ShowVisualMem	= from_memory;
}

// Categories exposed by the browser. Ids come straight from EChooseMode, so the
// fill/thumbnail delegates registered in FillChooseEvents are reused verbatim.
struct SCategory { u32 id; LPCSTR caption; bool placeable; };
// Levels have no EChooseMode - they are scenes, not library assets - so they get
// a sentinel id the browser special-cases when filling and when opening.
static const u32 kLevelsCategoryId = u32(-2);
static const SCategory kCategories[] =
{
	{ smObject,		"Objects",		true  },
	{ kLevelsCategoryId, "Levels",	false },
	{ smGroup,		"Groups",		false },
	{ smVisual,		"Visuals",		false },
	{ smTexture,	"Textures",		false },
	{ smTextureRaw,	"Textures (raw)",false },
	{ smPE,			"Particles",	false },
	{ smSoundSource,"Sounds",		false },
	{ smEntityType,	"Entities",		false },
	{ smLAnim,		"Light Anims",	false },
};
static const int kCategoryCount = sizeof(kCategories)/sizeof(kCategories[0]);

// Which files back an "Editor Content" item, per category. An item there is a
// library REFERENCE without an extension, so copying one into the project means
// resolving it against the fs.ltx aliases first. More than one entry per row is
// normal and wanted: a texture is only useful in a mod with its .thm beside it,
// and the uncompressed source is worth taking when it is there.
struct SLibFile { LPCSTR alias; LPCSTR ext; };
static const SLibFile kLibObject[]	= { { "$objects$",		".object"	} };
static const SLibFile kLibGroup[]	= { { "$groups$",		".group"	} };
static const SLibFile kLibVisual[]	= { { "$game_meshes$",	".ogf"		},
										{ "$game_meshes$",	".omf"		} };
static const SLibFile kLibTexture[]	= { { "$game_textures$",".dds"		},
										{ "$textures$",		".thm"		},
										{ "$textures$",		".tga"		} };
static const SLibFile kLibSound[]	= { { "$game_sounds$",	".ogg"		},
										{ "$sounds$",		".thm"		},
										{ "$sounds$",		".wav"		} };
static const SLibFile kLibLevel[]	= { { "$maps$",			".level"	} };

static const SLibFile* LibFilesFor(u32 category, int& count)
{
#define LIB_ROW(v)	do { count = sizeof(v)/sizeof(v[0]); return v; } while(0)
	if (category == kLevelsCategoryId)	LIB_ROW(kLibLevel);
	switch (category)
	{
	case smObject:		LIB_ROW(kLibObject);
	case smGroup:		LIB_ROW(kLibGroup);
	case smVisual:		LIB_ROW(kLibVisual);
	case smTexture:
	case smTextureRaw:	LIB_ROW(kLibTexture);
	case smSoundSource:	LIB_ROW(kLibSound);
	}
#undef LIB_ROW
	// particles, light animations and entity types live inside one shared file
	// each - there is no per-item file to hand over
	count = 0;
	return 0;
}

// keep at most this many live thumbnail textures
static const u32 kThumbBudget = 512;
// DARF only: a game install holds six figures of files, so one frame may
// decode this many new thumbnails and draw this many tiles. Both limits are
// progressive - the rest shows up on the following frames / after a search.
static const u32 kDarfThumbsPerFrame	= 16;
static const int kDarfMaxTiles			= 2000;

UIContentBrowser::UIContentBrowser()
{
	m_Category		= 0;
	m_Source		= 1;	// Editor Content by default until the project has assets
	m_NeedRefresh	= true;
	m_TileSize		= 96.f;
	m_Tick			= 0;
	m_ClipSource	= -1;
	m_ClipCategory	= u32(-1);
	m_Marquee		= false;
	m_MarqueeStart	= ImVec2(0, 0);
	// preferences may not exist yet when the browser is constructed at startup
	if (CLevelPreferences* prefs = dynamic_cast<CLevelPreferences*>(EPrefs))
		m_TreeWidth	= float(prefs->ContentBrowserTreeWidth);
	else
		m_TreeWidth	= 220.f;
	m_Root.name		= "Editor Content";
	m_Root.path		= "";
	m_DarfReady		= false;
	m_WantOverwrite	= false;
	m_ThumbsThisFrame = 0;
}

UIContentBrowser::~UIContentBrowser()
{
	DropCache();
}

void UIContentBrowser::Show()
{
	if (!Form) Form = xr_new<UIContentBrowser>();
	Form->bOpen = true;
}

void UIContentBrowser::Close()
{
	xr_delete(Form);
}

void UIContentBrowser::Update()
{
	if (!Form) return;
	Form->Draw();
	if (Form->IsClosed()) Close();
}

LPCSTR UIContentBrowser::DraggedAsset()
{
	return (Form && !Form->m_Dragged.empty()) ? Form->m_Dragged.c_str() : 0;
}

u32 UIContentBrowser::DraggedCategory()
{
	return Form ? kCategories[Form->m_Category].id : 0;
}

int UIContentBrowser::CategoryCount() { return kCategoryCount; }

LPCSTR UIContentBrowser::CategoryName(int index)
{
	return (index >= 0 && index < kCategoryCount) ? kCategories[index].caption : "";
}

u32 UIContentBrowser::CategoryId(int index)
{
	return (index >= 0 && index < kCategoryCount) ? kCategories[index].id : 0;
}

int UIContentBrowser::FindCategory(LPCSTR name)
{
	if (!name || !name[0]) return -1;
	for (int i = 0; i < kCategoryCount; ++i)
		if (0 == _stricmp(kCategories[i].caption, name))
			return i;
	return -1;
}

void UIContentBrowser::DropCache()
{
	for (ThumbMapIt it = m_Thumbs.begin(); it != m_Thumbs.end(); ++it)
		if (it->second.tex)
#if defined(USE_DX11)
			// ImTextureID is already the view type here - no cast needed
			it->second.tex->Release();
#else
			((IDirect3DBaseTexture9*)it->second.tex)->Release();
#endif
	m_Thumbs.clear();
}

//------------------------------------------------------------------------------
// data
//------------------------------------------------------------------------------
static void ScanContentDir(const char* base, const char* rel, ChooseItemVec& out)
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
			ScanContentDir(base, sub, out);
		else
			out.push_back(SChooseItem(sub, ""));
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
}

void UIContentBrowser::Refresh()
{
	m_NeedRefresh	= false;
	m_Items.clear	();
	DropCache		();

	if (m_Source == 0)
	{
		// The project's own content, scanned off disk. Both roots are listed:
		// gamedata is what the engine loads, rawdata what the editor compiles
		// from - and the copiers write into either depending on the asset, so
		// showing only one of them hid half of what had just been copied in.
		if (EditorProject::Active())
		{
			ScanContentDir(EditorProject::Root(), "gamedata", m_Items);
			ScanContentDir(EditorProject::Root(), "rawdata",  m_Items);
		}
		m_Root.name = "Content";
	}
	else if (m_Source == 2)
	{
		// linked game install: archives + loose gamedata as one virtual tree
		m_Root.name = "DARF Content";
		xr_string err;
		m_DarfReady = EditorGameContent::EnsureMounted(err);
		if (m_DarfReady)
		{
			m_DarfStatus = "";
			const int n = EditorGameContent::Count();
			m_Items.reserve(n);
			for (int i = 0; i < n; ++i)
				m_Items.push_back(SChooseItem(EditorGameContent::Get(i)->path.c_str(), ""));
		}
		else
		{
			m_DarfStatus  = "DARF Content is unavailable: ";
			m_DarfStatus += err;
			m_DarfStatus += ".";
		}
	}
	else if (kCategories[m_Category].id == kLevelsCategoryId)
	{
		// Levels are editor scenes, not library assets - they have no entry in
		// the choose-event table, so they are listed straight off the maps root.
		m_Root.name = "Editor Content";
		FS_FileSet lst;
		if (FS.file_list(lst, _maps_, FS_ListFiles | FS_ClampExt, "*.level"))
		{
			FS_FileSetIt it = lst.begin(), e = lst.end();
			for (; it != e; ++it)
				m_Items.push_back(SChooseItem(it->name.c_str(), ""));
		}
	}
	else
	{
		SChooseEvents* E = UIChooseForm::GetEvents(kCategories[m_Category].id);
		if (E && E->on_fill) E->on_fill(m_Items, 0);
		m_Root.name = "Editor Content";
	}

	BuildTree		();
}

void UIContentBrowser::SwitchSource(int src)
{
	if (m_Source == src) return;
	m_Source		= src;
	m_CurFolder		= "";
	ClearSelection();
	m_NeedRefresh	= true;
	m_WantOverwrite	= false;
	m_CopyPending.clear();
	m_CopyTarget.clear();
	if (src == 2)
	{
		// the first mount reads every archive file table and can take a moment
		m_DarfReady		= false;
		m_DarfStatus	= "mounting the linked game install...";
	}
}

void UIContentBrowser::BuildTree()
{
	m_Root.children.clear();
	m_Root.items.clear();

	for (int i = 0; i < (int)m_Items.size(); ++i)
	{
		LPCSTR nm = m_Items[i].name.c_str();
		if (!nm || !nm[0]) continue;

		// split "a\b\name" into folders, the tail is the item itself
		SFolder* cur = &m_Root;
		xr_string acc;
		LPCSTR p = nm;
		for (;;)
		{
			LPCSTR slash = strchr(p, '\\');
			if (!slash) break;

			string_path buf;
			int len = int(slash - p);
			if (len > (int)sizeof(buf) - 1) len = sizeof(buf) - 1;
			CopyMemory(buf, p, len);
			buf[len] = 0;

			xr_string part(buf);
			if (!acc.empty()) acc += "\\";
			acc += part;

			SFolder* next = 0;
			for (u32 c = 0; c < cur->children.size(); ++c)
				if (cur->children[c].name == part) { next = &cur->children[c]; break; }

			if (!next)
			{
				SFolder f; f.name = part; f.path = acc;
				cur->children.push_back(f);
				next = &cur->children.back();
			}
			cur = next;
			p = slash + 1;
		}
		cur->items.push_back(i);
	}
}

void UIContentBrowser::CollectItems(SFolder& f, xr_vector<int>& out, bool recursive)
{
	for (u32 i = 0; i < f.items.size(); ++i) out.push_back(f.items[i]);
	if (recursive)
		for (u32 c = 0; c < f.children.size(); ++c) CollectItems(f.children[c], out, true);
}

// Queues a model render for `name`, picking the right source: DARF items are
// only in the private archive VFS, project items only on disk, library visuals
// are resolvable by name.
void UIContentBrowser::RequestModelThumbnail(LPCSTR name)
{
	if (m_Source == 2)
	{
		const int idx = EditorGameContent::Find(name);
		u32 sz = 0;
		u8* bytes = (idx >= 0) ? EditorGameContent::ReadBytes(idx, sz) : 0;
		if (bytes && sz)
			QueueVisualThumbnailFromMemory(name, bytes, sz);
		else
		{
			// not readable from the archives - the editor ships its own copy of
			// the mesh tree, so try the same path there
			char visual[MAX_PATH];
			ToVisualName(name, visual, sizeof(visual));
			QueueVisualThumbnail(name, visual);
		}
		EditorGameContent::FreeBytes(bytes);
	}
	else if (m_Source == 0)
	{
		char abs[MAX_PATH];
		sprintf_s(abs, "%s\\%s", EditorProject::Root(), name);
		if (IReader* r = FS.r_open(abs))
		{
			QueueVisualThumbnailFromMemory(name, r->pointer(), (u32)r->length());
			FS.r_close(r);
		}
	}
	else
	{
		QueueVisualThumbnail(name, name);
	}
}

ImTextureID UIContentBrowser::GetThumb(LPCSTR name)
{
	shared_str key(name);
	ThumbMapIt it = m_Thumbs.find(key);
	if (it != m_Thumbs.end())
	{
		it->second.last_used = m_Tick;
		return it->second.tex;
	}

	ImTextureID tex = 0;

	// Models are never rendered here: the request goes into a queue that the
	// frame loop drains outside the scene (see EThumbnailVisual.h). Rendering
	// inline would mean closing the editor's scene from inside its own ImGui
	// pass, and any throw in between takes the next frame down with it.
	const bool needs_render = IsVisualExt(name);
	const bool will_render	= needs_render ||
							  (m_Source == 1 && kCategories[m_Category].id == smVisual);
	if (will_render)
	{
		U32Vec	pixels;
		bool	failed = false;
		if (TakeVisualThumbnail(name, pixels, failed))
		{
			// a failed render caches a null texture, so it is not asked for again
			tex = failed ? 0 : (ImTextureID)XFinedMCP::PixelsToTexture(pixels);
			SThumb t; t.tex = tex; t.last_used = m_Tick;
			m_Thumbs.insert(mk_pair(key, t));
			return tex;
		}
		RequestModelThumbnail(name);
		return 0;	// not cached: the tile retries until the queue answers
	}

	// image decoding stays inline - it is cheap - but the DARF tree is huge, so
	// it keeps its per-frame budget
	if (m_Source == 2 && m_ThumbsThisFrame >= kDarfThumbsPerFrame)
		return 0;

	if (m_Source == 2)
	{
		// archive-backed files have no disk path: pull the bytes through the
		// private VFS and let D3DX decode them. Unknown types cache a null
		// texture, so the tile falls back to a plain name button.
		const int idx = IsImageExt(name) ? EditorGameContent::Find(name) : -1;
		u32 sz = 0;
		u8* bytes = (idx >= 0) ? EditorGameContent::ReadBytes(idx, sz) : 0;
		if (bytes && sz)
		{
#if defined(USE_DX11)
			tex = DX11TextureFromMemory(bytes, sz);
#else
			IDirect3DTexture9* t = 0;
			if (SUCCEEDED(D3DXCreateTextureFromFileInMemoryEx(HW.pDevice, bytes, sz,
				D3DX_DEFAULT, D3DX_DEFAULT, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED,
				D3DX_FILTER_LINEAR, D3DX_FILTER_NONE, 0, NULL, NULL, &t)))
				tex = (ImTextureID)t;
#endif
		}
		EditorGameContent::FreeBytes(bytes);
		m_ThumbsThisFrame++;
	}
	else if (m_Source == 0)
	{
		if (IsImageExt(name))
		{
			// project files: direct image load for the formats the decoder knows
			char abs[MAX_PATH];
			sprintf_s(abs, "%s\\%s", EditorProject::Root(), name);
#if defined(USE_DX11)
			tex = DX11TextureFromFile(abs);
#else
			IDirect3DTexture9* t = 0;
			if (SUCCEEDED(D3DXCreateTextureFromFileExA(HW.pDevice, abs, D3DX_DEFAULT, D3DX_DEFAULT, 1, 0,
				D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, D3DX_FILTER_LINEAR, D3DX_FILTER_NONE, 0, NULL, NULL, &t)))
				tex = (ImTextureID)t;
#endif
		}
	}
	else
	{
		SChooseEvents* E = UIChooseForm::GetEvents(kCategories[m_Category].id);
		if (E && E->on_get_texture) E->on_get_texture(name, tex);

		// Roughly half of the shipped .object files have no baked .thm, so their
		// tiles stayed blank. Render those instead of giving up.
		if (!tex && kCategories[m_Category].id == smObject)
		{
			U32Vec	pixels;
			bool	failed = false;
			if (TakeVisualThumbnail(name, pixels, failed))
				tex = failed ? 0 : (ImTextureID)XFinedMCP::PixelsToTexture(pixels);
			else
			{
				QueueObjectThumbnail(name, name);
				return 0;	// retry next frame, do not cache a miss
			}
		}
	}

	// evict coldest entries once the budget is blown
	if (m_Thumbs.size() >= kThumbBudget)
	{
		u32 oldest = m_Tick;
		ThumbMapIt victim = m_Thumbs.end();
		for (ThumbMapIt i = m_Thumbs.begin(); i != m_Thumbs.end(); ++i)
			if (i->second.last_used <= oldest) { oldest = i->second.last_used; victim = i; }
		if (victim != m_Thumbs.end())
		{
#if defined(USE_DX11)
			if (victim->second.tex) victim->second.tex->Release();
#else
			if (victim->second.tex) ((IDirect3DBaseTexture9*)victim->second.tex)->Release();
#endif
			m_Thumbs.erase(victim);
		}
	}

	SThumb t; t.tex = tex; t.last_used = m_Tick;
	m_Thumbs.insert(mk_pair(key, t));
	return tex;
}

//------------------------------------------------------------------------------
// ui
//------------------------------------------------------------------------------
void UIContentBrowser::DrawFolder(SFolder& f)
{
	const bool leaf	= f.children.empty();
	int flags		= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (leaf)					flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (m_CurFolder == f.path)	flags |= ImGuiTreeNodeFlags_Selected;
	if (&f == &m_Root)			flags |= ImGuiTreeNodeFlags_DefaultOpen;

	const bool open = ImGui::TreeNodeEx(f.name.c_str(), flags);
	if (ImGui::IsItemClicked()) m_CurFolder = f.path;

	if (open && !leaf)
	{
		for (u32 i = 0; i < f.children.size(); ++i) DrawFolder(f.children[i]);
		ImGui::TreePop();
	}
}

// Double-click action, Unreal-style: open the asset, never modify the scene.
// Models get the preview window; kinds with a dedicated editor go there; the
// rest report that they have no viewer yet instead of doing something random.
void UIContentBrowser::OpenAsset(LPCSTR name)
{
	if (!name || !name[0]) return;

	// A level IS the scene: opening one loads it, which is the one case where
	// the double-click legitimately changes what the editor is working on.
	// COMMAND_LOAD asks about unsaved changes itself.
	if (m_Source == 1 && kCategories[m_Category].id == kLevelsCategoryId)
	{
		ExecCommand(COMMAND_LOAD, xr_string(name));
		return;
	}

	// Library objects (.object) are previewable too - the preview window loads
	// them through CEditableObject when the model pool has nothing under that
	// name. Without this the most-used category in the browser opened nothing.
	const bool is_model = IsVisualExt(name) ||
						  (m_Source == 1 && (kCategories[m_Category].id == smVisual ||
											 kCategories[m_Category].id == smObject));
	if (is_model && (s_ShowVisual || s_ShowVisualMem))
	{
		if (m_Source == 2 && s_ShowVisualMem)
		{
			const int idx = EditorGameContent::Find(name);
			u32 sz = 0;
			u8* bytes = (idx >= 0) ? EditorGameContent::ReadBytes(idx, sz) : 0;
			if (bytes && sz)		s_ShowVisualMem(name, bytes, sz);
			else if (s_ShowVisual)
			{
				char visual[MAX_PATH];
				ToVisualName(name, visual, sizeof(visual));
				s_ShowVisual(visual);
			}
			EditorGameContent::FreeBytes(bytes);
		}
		else if (m_Source == 0 && s_ShowVisualMem)
		{
			char abs[MAX_PATH];
			sprintf_s(abs, "%s\\%s", EditorProject::Root(), name);
			if (IReader* r = FS.r_open(abs))
			{
				s_ShowVisualMem(name, r->pointer(), (u32)r->length());
				FS.r_close(r);
			}
		}
		else if (s_ShowVisual)
			s_ShowVisual(name);
		return;
	}

	ELog.Msg(mtInformation, "No viewer for this asset kind yet: '%s'", name);
}

// depth-first lookup by full path; null when the path is not in the tree
UIContentBrowser::SFolder* UIContentBrowser::FindFolder(LPCSTR path)
{
	if (!path || !path[0]) return &m_Root;

	xr_vector<SFolder*> stack; stack.push_back(&m_Root);
	while (!stack.empty())
	{
		SFolder* f = stack.back(); stack.pop_back();
		if (f->path == path) return f;
		for (u32 i = 0; i < f->children.size(); ++i) stack.push_back(&f->children[i]);
	}
	return 0;
}

void UIContentBrowser::DrawTiles()
{
	// resolve the selected folder, falling back to root
	SFolder* cur = FindFolder(m_CurFolder.c_str());
	if (!cur) cur = &m_Root;

	// rebuilt every frame: a Shift-range means "everything between these two in
	// the order they are on screen", which only this pass knows. The rects go
	// with it, for the rubber-band.
	m_DrawnOrder.clear();
	m_DrawnRects.clear();

	xr_vector<int> ids;
	// with an active search the whole subtree is scanned, otherwise just this folder
	CollectItems(*cur, ids, m_Filter.IsActive());

	const float cell	= m_TileSize + ImGui::GetStyle().ItemSpacing.x;
	const float avail	= ImGui::GetContentRegionAvail().x;
	int per_row			= (int)(avail / cell);
	if (per_row < 1) per_row = 1;

	int drawn = 0, matched = 0;

	// Folders come first, like Unreal: the grid is a view of the folder, so
	// what is inside it - subfolders included - has to be reachable from here
	// and not only from the tree. A search flattens the subtree, so folder
	// tiles are pointless (and misleading) while the filter is active.
	if (!m_Filter.IsActive())
	{
		if (!m_CurFolder.empty())
		{
			// step out: the parent path is everything before the last separator
			ImGui::PushID("##cb_up");
			ImGui::Button("..", ImVec2(m_TileSize + 8.f, m_TileSize + 8.f));
			// double click, same as every other folder tile - a single click
			// here navigated while clicking a folder did not, which is worse
			// than either rule on its own
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				LPCSTR cut = strrchr(m_CurFolder.c_str(), '\\');
				if (cut)	m_CurFolder.erase(size_t(cut - m_CurFolder.c_str()));
				else		m_CurFolder.clear();
				ClearSelection();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("up one folder (double click)");
			ImGui::PopID();
			drawn++;
		}

		for (u32 c = 0; c < cur->children.size(); ++c)
		{
			SFolder& sub = cur->children[c];
			if (drawn % per_row) ImGui::SameLine();
			drawn++;

			ImGui::PushID(1000000 + (int)c);
			ImGui::BeginGroup();
			// folders read as folders through colour: there is no icon atlas here
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.27f, 0.14f, 1.f));
			ImGui::Button("[ ]", ImVec2(m_TileSize, m_TileSize));
			ImGui::PopStyleColor();
			// Double click to enter, like Unreal - a single click must not
			// navigate, or selecting a folder is impossible.
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				m_CurFolder = sub.path;
				ClearSelection();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sub.path.c_str());
			DrawFolderContextMenu(sub.path.c_str());

			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + m_TileSize);
			ImGui::TextUnformatted(sub.name.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndGroup();
			ImGui::PopID();
		}
	}

	for (u32 k = 0; k < ids.size(); ++k)
	{
		SChooseItem& it	= m_Items[ids[k]];
		LPCSTR full		= it.name.c_str();
		if (!m_Filter.PassFilter(full)) continue;
		++matched;
		// a game install can match tens of thousands of files at once; drawing
		// them all would stall the frame, so the rest waits for a tighter search.
		// Only the game tree is that big - the SDK library draws in full.
		if (IsGameSource() && drawn >= kDarfMaxTiles) continue;

		LPCSTR leaf		= strrchr(full, '\\');
		leaf			= leaf ? leaf + 1 : full;

		if (drawn % per_row) ImGui::SameLine();
		drawn++;

		ImGui::PushID(ids[k]);
		ImGui::BeginGroup();

		// the grid order is what a Shift-range means by "everything between"
		m_DrawnOrder.push_back(full);
		const u32 tile_slot = u32(m_DrawnRects.size());
		m_DrawnRects.push_back(ImVec4(0, 0, 0, 0));	// filled right after the widget

		ImTextureID tex	= GetThumb(full);
		const bool sel	= IsSelected(full);

		bool clicked;
		if (tex)	clicked = ImGui::ImageButton(tex, ImVec2(m_TileSize, m_TileSize));
		else		clicked = ImGui::Button(leaf, ImVec2(m_TileSize + 8.f, m_TileSize + 8.f));

		{
			const ImVec2 ra = ImGui::GetItemRectMin();
			const ImVec2 rb = ImGui::GetItemRectMax();
			m_DrawnRects[tile_slot] = ImVec4(ra.x, ra.y, rb.x, rb.y);
		}

		// Selection has to be visible ON the thumbnail: tinting ImGuiCol_Button
		// only colours the few pixels of frame the image does not cover, which
		// is why selecting looked like nothing happened. Unreal draws a wash
		// plus a bright border over the tile - same here, on the foreground
		// draw list so it lands above the image.
		if (sel)
		{
			const ImVec2	a	= ImGui::GetItemRectMin();
			const ImVec2	b	= ImGui::GetItemRectMax();
			ImDrawList*		dl	= ImGui::GetWindowDrawList();
			const ImVec4	hl	= ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
			dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(hl.x, hl.y, hl.z, 0.35f)), 3.f);
			dl->AddRect      (a, b, ImGui::GetColorU32(ImVec4(0.30f, 0.65f, 1.00f, 1.f)), 3.f, 0, 2.5f);
		}

		if (clicked)
		{
			const ImGuiIO& io = ImGui::GetIO();
			SelectItem(full, io.KeyCtrl, io.KeyShift);
		}

		// Drag source for every source, not just the SDK library: dropping a
		// game or project asset on the viewport has to work the same way.
		// Dragging an unselected tile selects it first, as Unreal does, so the
		// payload always matches what is highlighted.
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			if (!IsSelected(full)) SelectItem(full, false, false);
			m_Dragged = full;
			ImGui::SetDragDropPayload(CB_DND_PAYLOAD, full, xr_strlen(full) + 1);
			if (tex) ImGui::Image(tex, ImVec2(48, 48));
			ImGui::TextUnformatted(leaf);
			if (m_Selection.size() > 1)
				ImGui::Text("and %d more", int(m_Selection.size()) - 1);
			ImGui::EndDragDropSource();
		}

		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", full);
			// Unreal semantics: a double click OPENS the asset, it never places
			// it. Putting something on the level is drag&drop onto the viewport
			// and nothing else - a stray double click must not edit the scene.
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				SelectItem(full, false, false);
				OpenAsset(full);
			}
		}

		// Right-click surface. Drawn last so the popup contents never become the
		// "last item" the hover check above reads.
		DrawItemContextMenu(full);

		// caption under the tile, clipped to tile width
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + m_TileSize);
		ImGui::TextUnformatted(leaf);
		ImGui::PopTextWrapPos();

		ImGui::EndGroup();
		ImGui::PopID();
	}

	// Shift-click needed the whole grid order, so it is resolved here
	ApplyPendingRange();

	// empty-space right-click and the Ctrl shortcuts, both of which need the
	// drawn order the loop above just produced
	DrawGridContextMenu();
	HandleShortcuts();

	// clicking empty space clears the selection, as every file browser does -
	// but only a bare click, so it does not fight the rubber-band below
	if (!m_Marquee &&
		ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
		!ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() &&
		ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
		!ImGui::GetIO().KeyCtrl)
		ClearSelection();

	UpdateMarquee();

	if (!drawn) ImGui::TextDisabled(m_Filter.IsActive() ? "nothing matches the search" : "empty folder");
	else if (matched > drawn)
	{
		ImGui::Dummy(ImVec2(0, 4));
		ImGui::TextDisabled("... and %d more - narrow the search", matched - drawn);
	}
}

//------------------------------------------------------------------------------
// DARF actions
//
// Read-only contract, enforced here: the menu exposes browse/preview, a copy
// INTO the project and (when it resolves) placement. There is deliberately no
// rename, delete, move, create or drag-out entry, and every path that writes
// goes through EditorGameContent::CopyToProject, whose destination is always
// below <project>\gamedata\.
//------------------------------------------------------------------------------
void UIContentBrowser::DrawItemContextMenu(LPCSTR full)
{
	if (!ImGui::BeginPopupContextItem("cb_item_ctx")) return;

	// right-clicking outside the selection moves it here, as Unreal does;
	// right-clicking inside keeps the multi-selection intact
	if (!IsSelected(full)) SelectItem(full, false, false);

	const int count = int(m_Selection.size());
	if (count > 1)	ImGui::TextDisabled("%d items selected", count);
	else			ImGui::TextDisabled("%s", full);
	ImGui::Separator();

	if (ImGui::MenuItem("Open")) OpenAsset(full);

	if (IsReadOnlySource())
	{
		int n = 0;
		// Editor Content items are library refs; a category with no file of its
		// own (particles, light anims) has nothing to hand over
		const bool copyable = IsGameSource() || !!LibFilesFor(kCategories[m_Category].id, n);
		if (ImGui::MenuItem(count > 1 ? "Copy selection to project" : "Copy to project",
							"", false, copyable) && copyable)
			CopySelection();
		if (!copyable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("this category lives inside one shared library file - there is no per-item file to copy");
	}

	string_path ref;
	const bool placeable = IsGameSource()
		? EditorGameContent::ResolvePlaceable(full, ref)
		: false;
	if (IsGameSource())
	{
		if (ImGui::MenuItem("Place in scene", "", false, placeable) && placeable)
			ExecCommand(COMMAND_CB_PLACE_ASSET, xr_string(ref), u32(0));
	}
	else if (ImGui::MenuItem("Place in scene"))
		ExecCommand(COMMAND_CB_PLACE_ASSET, xr_string(full), u32(0));

	ImGui::Separator();
	if (ImGui::MenuItem("Copy", "Ctrl+C")) ClipboardCopy();
	{
		const bool can = CanPaste();
		if (ImGui::MenuItem("Paste", "Ctrl+V", false, can) && can) ClipboardPaste();
		if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", PasteBlockedReason());
	}
	if (ImGui::MenuItem("Copy name")) ImGui::SetClipboardText(full);
	if (IsGameSource() && !placeable)
		ImGui::TextDisabled("  not a library object reference");

	if (IsReadOnlySource())
	{
		ImGui::Separator();
		ImGui::TextDisabled("read-only source");
	}
	ImGui::EndPopup();
}

//------------------------------------------------------------------------------
// Folder right-click. Copying a whole folder needs a recursive copier the
// backend does not have yet, so the entry is present but disabled rather than
// silently missing - it says what it will do and why it cannot yet.
//------------------------------------------------------------------------------
void UIContentBrowser::DrawFolderContextMenu(LPCSTR path)
{
	if (!ImGui::BeginPopupContextItem("cb_folder_ctx")) return;

	ImGui::TextDisabled("%s", path);
	ImGui::Separator();

	if (ImGui::MenuItem("Open"))
	{
		m_CurFolder = path;
		ClearSelection();
	}
	if (ImGui::MenuItem("Select contents"))
	{
		// selects what this folder holds, so the item copy can take it
		if (SFolder* f = FindFolder(path))
		{
			xr_vector<int> ids;
			CollectItems(*f, ids, true);
			m_Selection.clear();
			for (u32 i = 0; i < ids.size(); ++i)
				m_Selection.push_back(m_Items[ids[i]].name.c_str());
			if (!m_Selection.empty()) m_Anchor = m_Selection.back();
		}
	}

	ImGui::Separator();
	{
		int n = 0;
		const bool copyable = IsReadOnlySource() &&
			(IsGameSource() || !!LibFilesFor(kCategories[m_Category].id, n));
		if (ImGui::MenuItem("Copy folder to project (recursive)", "", false, copyable) && copyable)
			CopyFolderToProject(path);
		if (!copyable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(IsReadOnlySource()
				? "this category lives inside one shared library file - there is no per-item file to copy"
				: "this is the project's own Content - it is already here");
	}

	if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(path);

	if (IsReadOnlySource())
	{
		ImGui::Separator();
		ImGui::TextDisabled("read-only source");
	}
	ImGui::EndPopup();
}

//------------------------------------------------------------------------------
// MCP entry points
//------------------------------------------------------------------------------
bool UIContentBrowser::RevealAsset(LPCSTR name, int source, bool open_viewer, xr_string& err)
{
	if (!name || !name[0])	{ err = "empty asset name"; return false; }
	Show();									// the panel has to exist to show anything
	if (!Form)				{ err = "content browser unavailable"; return false; }

	if (source >= 0 && source <= 2 && source != Form->m_Source)
		Form->SwitchSource(source);

	// the listing is rebuilt lazily; the lookup below needs it now
	if (Form->m_NeedRefresh) Form->Refresh();

	int found = -1;
	for (u32 i = 0; i < Form->m_Items.size(); ++i)
		if (0 == _stricmp(Form->m_Items[i].name.c_str(), name)) { found = int(i); break; }

	if (found < 0)
	{
		// the same name may live under a different category (Objects/Textures/…);
		// say so plainly rather than silently doing nothing
		err = "asset not found in the current source/category";
		return false;
	}

	// navigate to the folder that holds it, clear any search that would hide it
	LPCSTR	cut = strrchr(name, '\\');
	Form->m_CurFolder	= cut ? xr_string(name).substr(0, size_t(cut - name)) : xr_string("");
	Form->m_Filter.Clear();

	Form->m_Selection.clear();
	Form->m_Selection.push_back(name);
	Form->m_Anchor = name;

	if (open_viewer) Form->OpenAsset(name);
	return true;
}

void UIContentBrowser::GetSelection(int& source, xr_string& folder, xr_vector<xr_string>& sel)
{
	source = Form ? Form->m_Source : -1;
	folder = Form ? Form->m_CurFolder : xr_string("");
	sel.clear();
	if (Form) sel = Form->m_Selection;
}

bool UIContentBrowser::McpCopyToProject(LPCSTR names, LPCSTR folder, int source, int category,
										bool overwrite, int& files, xr_string& err)
{
	files	= 0;
	err		= "";

	if (!Form) Form = xr_new<UIContentBrowser>();

	if (source >= 0 && source != Form->m_Source)	Form->SwitchSource(source);
	if (category >= 0)
	{
		if (category >= kCategoryCount)	{ err = "unknown category"; return false; }
		if (u32(category) != Form->m_Category)
		{
			Form->m_Category	= u32(category);
			Form->m_CurFolder	= "";
			Form->m_NeedRefresh	= true;
		}
	}
	if (Form->m_Source == 0)	{ err = "the project's own Content is a destination, not a source"; return false; }

	// the listing has to be current: the tree is what turns a folder into the
	// set of items under it, and a source switch leaves it stale
	if (Form->m_NeedRefresh) Form->Refresh();

	xr_vector<xr_string> list;
	if (folder && folder[0])
	{
		string_path norm;
		xr_strcpy	(norm, sizeof(norm), folder);
		for (char* p = norm; *p; ++p) if ('/' == *p) *p = '\\';

		SFolder* f = Form->FindFolder(norm);
		if (!f)	{ err = "no such folder in this source"; return false; }

		xr_vector<int> ids;
		Form->CollectItems(*f, ids, true);
		list.reserve(ids.size());
		for (u32 i = 0; i < ids.size(); ++i)
			list.push_back(Form->m_Items[ids[i]].name.c_str());
	}
	else if (names && names[0])
	{
		// ';'-separated, same convention the file-op commands use
		string_path buf;
		xr_strcpy	(buf, sizeof(buf), names);
		for (char* cursor = buf; cursor && *cursor;)
		{
			char* sep = strchr(cursor, ';');
			if (sep) *sep = 0;
			while (' ' == *cursor) ++cursor;
			for (char* e = cursor + xr_strlen(cursor); e > cursor && ' ' == e[-1];) *--e = 0;
			for (char* p = cursor; *p; ++p) if ('/' == *p) *p = '\\';
			if (*cursor) list.push_back(cursor);
			cursor = sep ? sep + 1 : 0;
		}
	}

	if (list.empty())	{ err = "nothing to copy - pass 'names' or 'folder'"; return false; }

	files = Form->CopyRefsToProject(kCategories[Form->m_Category].id, list, overwrite, err);
	Form->m_NeedRefresh = true;
	return err.empty();
}

//------------------------------------------------------------------------------
// selection
//------------------------------------------------------------------------------
bool UIContentBrowser::IsSelected(LPCSTR full) const
{
	for (u32 i = 0; i < m_Selection.size(); ++i)
		if (m_Selection[i] == full)	return true;
	return false;
}

void UIContentBrowser::ClearSelection()
{
	m_Selection.clear();
	m_Anchor.clear();
	m_PendingRange.clear();
}

void UIContentBrowser::SelectItem(LPCSTR full, bool additive, bool range)
{
	if (range && !m_Anchor.empty())
	{
		// resolved after the grid, where the drawn order is known
		m_PendingRange = full;
		return;
	}

	if (additive)
	{
		for (u32 i = 0; i < m_Selection.size(); ++i)
			if (m_Selection[i] == full)
			{
				m_Selection.erase(m_Selection.begin() + i);
				// the anchor must stay on something that is still selected
				if (m_Anchor == full)
					m_Anchor = m_Selection.empty() ? xr_string("") : m_Selection.back();
				return;
			}
		m_Selection.push_back(full);
		m_Anchor = full;
		return;
	}

	m_Selection.clear();
	m_Selection.push_back(full);
	m_Anchor = full;
}

//------------------------------------------------------------------------------
// Rubber-band selection. Starts on empty grid space (a press over a tile is that
// tile's click, or the start of a drag&drop), extends while held, and selects
// everything the band touches. Ctrl keeps what was already selected, which is
// how Unreal and every file manager behave.
//------------------------------------------------------------------------------
void UIContentBrowser::UpdateMarquee()
{
	const ImGuiIO&	io		= ImGui::GetIO();
	const bool		inside	= ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

	if (!m_Marquee)
	{
		// only empty space starts a band, and never while a drag&drop is live
		if (inside && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() &&
			ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			m_Marquee		= true;
			m_MarqueeStart	= io.MousePos;
			m_MarqueeBase	= io.KeyCtrl ? m_Selection : xr_vector<xr_string>();
		}
		return;
	}

	const ImVec2 a(_min(m_MarqueeStart.x, io.MousePos.x), _min(m_MarqueeStart.y, io.MousePos.y));
	const ImVec2 b(_max(m_MarqueeStart.x, io.MousePos.x), _max(m_MarqueeStart.y, io.MousePos.y));

	// a click without movement is not a band - let the empty-space click clear
	const bool dragged = (b.x - a.x) > 3.f || (b.y - a.y) > 3.f;

	if (dragged)
	{
		m_Selection = m_MarqueeBase;
		for (u32 i = 0; i < m_DrawnRects.size() && i < m_DrawnOrder.size(); ++i)
		{
			const ImVec4& r = m_DrawnRects[i];
			// plain AABB overlap: touched counts as selected
			if (r.z < a.x || r.x > b.x || r.w < a.y || r.y > b.y)	continue;
			if (!IsSelected(m_DrawnOrder[i].c_str()))
				m_Selection.push_back(m_DrawnOrder[i]);
		}
		if (!m_Selection.empty()) m_Anchor = m_Selection.back();

		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(0.30f, 0.65f, 1.00f, 0.20f)));
		dl->AddRect      (a, b, ImGui::GetColorU32(ImVec4(0.30f, 0.65f, 1.00f, 0.90f)));
	}

	if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		m_Marquee = false;
		m_MarqueeBase.clear();
	}
}

void UIContentBrowser::ApplyPendingRange()
{
	if (m_PendingRange.empty())	return;

	int from = -1, to = -1;
	for (u32 i = 0; i < m_DrawnOrder.size(); ++i)
	{
		if (m_DrawnOrder[i] == m_Anchor)		from = int(i);
		if (m_DrawnOrder[i] == m_PendingRange)	to	 = int(i);
	}
	m_PendingRange.clear();
	if (from < 0 || to < 0)	return;
	if (from > to)			std::swap(from, to);

	m_Selection.clear();
	for (int i = from; i <= to; ++i)
		m_Selection.push_back(m_DrawnOrder[i]);
	// the anchor stays put so dragging the range back and forth works
}

//------------------------------------------------------------------------------
// clipboard
//
// Ordinary file-manager semantics: copy marks what is selected and where it
// came from, paste materialises it in the folder you are looking at. Only the
// project's own Content can receive a paste - the SDK library and the game
// install are sources, never destinations.
//------------------------------------------------------------------------------
void UIContentBrowser::ClipboardCopy()
{
	if (m_Selection.empty()) return;
	m_Clipboard		= m_Selection;
	m_ClipSource	= m_Source;
	m_ClipCategory	= kCategories[m_Category].id;
	ELog.Msg(mtInformation, "Copied %d item(s).", int(m_Clipboard.size()));
}

bool UIContentBrowser::CanPaste() const
{
	if (m_Clipboard.empty())		return false;
	if (m_Source != 0)				return false;	// destination must be writable
	if (m_ClipSource == 0)			return false;	// project -> project is a move, not this
	if (m_ClipSource == 1)
	{
		int n = 0;
		return !!LibFilesFor(m_ClipCategory, n);
	}
	return true;
}

LPCSTR UIContentBrowser::PasteBlockedReason() const
{
	if (m_Clipboard.empty())	return "clipboard is empty";
	if (m_Source != 0)			return "paste only into the project's Content - this source is read-only";
	if (m_ClipSource == 0)		return "these items are already in the project";
	if (m_ClipSource == 1)
	{
		int n = 0;
		if (!LibFilesFor(m_ClipCategory, n))
			return "that category lives inside one shared library file - there is no per-item file to copy";
	}
	return "";
}

void UIContentBrowser::ClipboardPaste()
{
	if (!CanPaste()) return;

	// Both copiers mirror the item's own location - the game one under the
	// project's gamedata, the library one under whichever root fs.ltx puts that
	// alias in. The paste lands where the engine expects to find it rather than
	// in whatever folder happens to be open, which is what a mod needs.
	if (m_ClipSource == 1)
	{
		int files = 0, failed = 0;
		xr_string last_err;
		for (u32 i = 0; i < m_Clipboard.size(); ++i)
		{
			xr_string err;
			files += CopyLibraryItem(m_ClipCategory, m_Clipboard[i].c_str(), true, err);
			if (!err.empty())	{ ++failed; last_err = err; }
		}
		if (failed)	ELog.DlgMsg(mtError, "Pasted %d file(s); %d item(s) failed - %s",
								files, failed, last_err.c_str());
		else		ELog.Msg(mtInformation, "Pasted %d file(s).", files);
	}
	else
	{
		for (u32 i = 0; i < m_Clipboard.size(); ++i)
		{
			if (m_WantOverwrite) break;		// a conflict popup owns the rest
			RequestCopy(m_Clipboard[i].c_str());
		}
	}
	m_NeedRefresh = true;
}

void UIContentBrowser::HandleShortcuts()
{
	// only when this panel owns the keyboard, or Ctrl+C in the viewport would
	// end up copying assets
	if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;

	const ImGuiIO& io = ImGui::GetIO();
	if (!io.KeyCtrl) return;

	if (ImGui::IsKeyPressed(ImGuiKey_C, false))	ClipboardCopy();
	if (ImGui::IsKeyPressed(ImGuiKey_V, false))	ClipboardPaste();
	if (ImGui::IsKeyPressed(ImGuiKey_A, false))
	{
		m_Selection.clear();
		for (u32 i = 0; i < m_DrawnOrder.size(); ++i)
			m_Selection.push_back(m_DrawnOrder[i]);
		if (!m_Selection.empty()) m_Anchor = m_Selection.back();
	}
}

void UIContentBrowser::DrawGridContextMenu()
{
	// "Window" variant: fires on empty space, not over a tile
	if (!ImGui::BeginPopupContextWindow("cb_grid_ctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		return;

	const bool can = CanPaste();
	if (ImGui::MenuItem("Paste", "Ctrl+V", false, can) && can)
		ClipboardPaste();
	if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", PasteBlockedReason());

	if (!m_Clipboard.empty())
		ImGui::TextDisabled("%d item(s) on the clipboard", int(m_Clipboard.size()));

	ImGui::Separator();
	if (ImGui::MenuItem("Select all", "Ctrl+A"))
	{
		m_Selection.clear();
		for (u32 i = 0; i < m_DrawnOrder.size(); ++i)
			m_Selection.push_back(m_DrawnOrder[i]);
		if (!m_Selection.empty()) m_Anchor = m_Selection.back();
	}
	if (ImGui::MenuItem("Clear selection", "", false, !m_Selection.empty()))
		ClearSelection();
	if (ImGui::MenuItem("Refresh")) m_NeedRefresh = true;

	ImGui::EndPopup();
}

int UIContentBrowser::CopyLibraryItem(u32 category, LPCSTR ref, bool overwrite, xr_string& err)
{
	err = "";

	int n = 0;
	const SLibFile* files = LibFilesFor(category, n);
	if (!files || !n)
	{
		err = "this category has no file of its own - it lives inside a shared library file";
		return 0;
	}

	string_path base;
	xr_strcpy	(base, sizeof(base), ref);
	// library references come without an extension, but a search result or a
	// hand-typed name may carry one - strip it so the probes below own the ext
	if (char* dot = strrchr(base, '.'))
		if (!strchr(dot, '\\')) *dot = 0;

	EditorFileOps::SReport rep;
	int hits = 0;
	for (int i = 0; i < n; ++i)
	{
		string_path rel;
		xr_sprintf	(rel, sizeof(rel), "%s%s", base, files[i].ext);
		if (EditorFileOps::CopyLibraryFile(files[i].alias, rel, overwrite, rep))
			++hits;
	}

	if (!hits)								err = "no file behind this reference";
	else if (!rep.ok() && !rep.failures.empty())	err = rep.failures[0].reason;
	return rep.files;
}

void UIContentBrowser::CopySelection()
{
	if (m_Selection.empty())	return;

	if (IsGameSource())
	{
		// one confirmation per run: RequestCopy queues the first conflict and the
		// popup drives the rest on the next frames
		for (u32 i = 0; i < m_Selection.size(); ++i)
		{
			if (m_WantOverwrite)	break;
			RequestCopy(m_Selection[i].c_str());
		}
		return;
	}

	if (m_Source != 1)	return;		// the project's own Content is a target, not a source

	// Editor Content overwrites outright. Unlike the game install this library
	// is the editor's own, and the project holds a fresh mirror of whatever was
	// pulled into it - a per-file prompt while copying a folder full of props
	// would be unusable, and the report below says exactly what was written.
	xr_string err;
	const int files = CopyRefsToProject(kCategories[m_Category].id, m_Selection, true, err);

	m_NeedRefresh = true;
	if (!err.empty())	ELog.DlgMsg(mtError, "Copied %d file(s), with failures - %s", files, err.c_str());
	else				ELog.Msg(mtInformation, "Copied %d file(s) into the project.", files);
}

int UIContentBrowser::CopyRefsToProject(u32 category, const xr_vector<xr_string>& names,
										bool overwrite, xr_string& err)
{
	err = "";

	int files = 0, failed = 0;
	xr_string last_err;
	for (u32 i = 0; i < names.size(); ++i)
	{
		xr_string one;
		if (m_Source == 2)
		{
			// the game install keeps its own copier: the source may be a file
			// inside an archive, which only that one can read
			string_path target = {};
			bool existed = false;
			if (EditorGameContent::CopyToProject(names[i].c_str(), overwrite, target, existed, one))
				++files;
			else if (existed && !overwrite)
				one = "already exists (pass overwrite)";
			if (!one.empty())	{ ++failed; last_err = one; }
		}
		else
		{
			files += CopyLibraryItem(category, names[i].c_str(), overwrite, one);
			if (!one.empty())	{ ++failed; last_err = one; }
		}
	}

	if (failed)
	{
		char head[128];
		sprintf_s(head, sizeof(head), "%d item(s) failed, first: ", failed);
		err  = head;
		err += last_err;
	}
	return files;
}

void UIContentBrowser::CopyFolderToProject(LPCSTR path)
{
	SFolder* f = FindFolder(path);
	if (!f)	return;

	xr_vector<int> ids;
	CollectItems(*f, ids, true);
	if (ids.empty())
	{
		ELog.Msg(mtInformation, "'%s' holds no items.", path);
		return;
	}

	// route through the selection copier so both read-only sources keep one
	// code path, the game install's overwrite prompt included
	xr_vector<xr_string> saved;	saved.swap(m_Selection);
	m_Selection.reserve(ids.size());
	for (u32 i = 0; i < ids.size(); ++i)
		m_Selection.push_back(m_Items[ids[i]].name.c_str());

	CopySelection();
	m_Selection.swap(saved);
}

void UIContentBrowser::RequestCopy(LPCSTR rel)
{
	string_path target = {};
	bool existed = false;
	xr_string err;
	if (EditorGameContent::CopyToProject(rel, false, target, existed, err))
	{
		ELog.Msg(mtInformation, "Copied to '%s'.", target);
		return;
	}
	if (existed)
	{
		// never overwrite silently - ask first
		m_CopyPending	= rel;
		m_CopyTarget	= target;
		m_WantOverwrite	= true;
		return;
	}
	ELog.DlgMsg(mtError, "Copy failed: %s", err.c_str());
}

void UIContentBrowser::DrawCopyConfirm()
{
	if (!m_WantOverwrite) return;

	ImGui::OpenPopup("DARF - file exists");
	ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_FirstUseEver);
	if (ImGui::BeginPopupModal("DARF - file exists", &m_WantOverwrite))
	{
		ImGui::TextWrapped("The project already contains this file:");
		ImGui::TextDisabled("%s", m_CopyTarget.c_str());
		ImGui::Separator();
		if (ImGui::Button("Overwrite", ImVec2(140, 0)))
		{
			string_path target = {};
			bool existed = false;
			xr_string err;
			if (EditorGameContent::CopyToProject(m_CopyPending.c_str(), true, target, existed, err))
				ELog.Msg(mtInformation, "Copied to '%s'.", target);
			else
				ELog.DlgMsg(mtError, "Copy failed: %s", err.c_str());
			m_WantOverwrite = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine(0, 24);
		if (ImGui::Button("Keep existing", ImVec2(140, 0)))
		{
			ELog.Msg(mtInformation, "already exists: '%s'", m_CopyTarget.c_str());
			m_WantOverwrite = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

//------------------------------------------------------------------------------
// splitter between the folder tree and the tile grid
//
// An InvisibleButton is the whole handle: it takes the mouse capture, so the
// drag keeps working while the cursor runs ahead of the pane, and it swallows
// the click so neither child sees it. The grab area is wider than the drawn
// line - a 1px target is unusable.
//------------------------------------------------------------------------------
void UIContentBrowser::DrawSplitter(float height)
{
	const float grab	= 6.f;
	const float spacing	= ImGui::GetStyle().ItemSpacing.x;

	ImGui::SameLine(0.f, 0.f);
	const ImVec2 top = ImGui::GetCursorScreenPos();

	ImGui::InvisibleButton("##cb_split", ImVec2(grab + spacing, height > 0.f ? height : 1.f));
	const bool active	= ImGui::IsItemActive();
	const bool hovered	= ImGui::IsItemHovered();

	if (active)
	{
		m_TreeWidth += ImGui::GetIO().MouseDelta.x;

		// Total inner width, NOT GetContentRegionAvail(): the cursor has already
		// wrapped past the button by now, so "avail" would report the full width
		// again and the clamp below would let the tile pane be squeezed to zero.
		const float inner	= ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
		const float tiles	= 160.f + grab + spacing;	// one tile column + the handle
		const float max_w	= (inner > tiles) ? (inner - tiles) : 120.f;
		if (m_TreeWidth > max_w)	m_TreeWidth = max_w;
		if (m_TreeWidth < 120.f)	m_TreeWidth = 120.f;

		if (CLevelPreferences* prefs = dynamic_cast<CLevelPreferences*>(EPrefs))
			prefs->ContentBrowserTreeWidth = u32(m_TreeWidth);
	}
	if (active || hovered)
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

	// visible grip: separator tint at rest, the usual accent while dragged
	const ImU32 col = ImGui::GetColorU32(active	 ? ImGuiCol_SeparatorActive
									   : hovered ? ImGuiCol_SeparatorHovered
												 : ImGuiCol_Separator);
	const float x = top.x + (grab + spacing) * 0.5f;
	ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x - 1.f, top.y),
											  ImVec2(x + 1.f, top.y + ImGui::GetItemRectSize().y), col);

	ImGui::SameLine(0.f, 0.f);
}

void UIContentBrowser::Draw()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(420, 260));
	if (!ImGui::Begin("Content Browser", &bOpen))
	{
		ImGui::PopStyleVar(1);
		ImGui::End();
		return;
	}

	m_Tick++;
	m_ThumbsThisFrame = 0;
	if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left)) m_Dragged.clear();
	if (m_NeedRefresh) Refresh();

	// toolbar: UE-style source switch, then the category combo for the shared library
	{
		if (ImGui::RadioButton("Content", m_Source == 0))			SwitchSource(0);
		ImGui::SameLine();
		if (ImGui::RadioButton("Editor Content", m_Source == 1))	SwitchSource(1);
		ImGui::SameLine();
		if (ImGui::RadioButton("DARF Content", m_Source == 2))		SwitchSource(2);
	}
	ImGui::SameLine(0, 16);
	if (m_Source == 1)
	{
		ImGui::SetNextItemWidth(160);
		if (ImGui::BeginCombo("##cat", kCategories[m_Category].caption))
		{
			for (int i = 0; i < kCategoryCount; ++i)
				if (ImGui::Selectable(kCategories[i].caption, m_Category == (u32)i))
				{
					m_Category		= i;
					m_CurFolder		= "";
					ClearSelection();
					m_NeedRefresh	= true;
				}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
	}
	if (ImGui::Button("Refresh")) m_NeedRefresh = true;
	ImGui::SameLine();
	m_Filter.Draw("##filter", 180);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120);
	ImGui::SliderFloat("##tile", &m_TileSize, 48.f, 192.f, "%.0f");
	ImGui::SameLine();
	ImGui::TextDisabled("%d items", (int)m_Items.size());

	if (IsReadOnlySource())
	{
		ImGui::SameLine(0, 16);
		if (IsGameSource())
		{
			ImGui::BeginDisabled(m_Selection.empty());
			if (ImGui::Button(m_Selection.size() > 1 ? "Copy selection to project" : "Copy to project"))
				CopySelection();
			ImGui::EndDisabled();
			ImGui::SameLine();
		}
		ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "READ ONLY");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(IsGameSource()
				? "the linked game install is never modified: browse, preview and copy into the project only"
				: "the shared editor library is never modified: browse and preview only");
	}
	if (!m_Selection.empty())
	{
		ImGui::SameLine(0, 16);
		ImGui::TextDisabled("%d selected", int(m_Selection.size()));
	}

	ImGui::Separator();

	// no project / no game link: one explaining line instead of an empty tree.
	// IsGameSource, not IsReadOnlySource: the SDK library is read-only too but
	// has nothing to do with the game install being linked.
	if (IsGameSource() && !m_DarfReady)
	{
		ImGui::TextWrapped("%s", m_DarfStatus.c_str());
		ImGui::TextDisabled("Open a project and link a game install (fsgame.ltx + database\\) to browse it here.");
		ImGui::PopStyleVar(1);
		ImGui::End();
		return;
	}

	const float pane_h = ImGui::GetContentRegionAvail().y;

	if (ImGui::BeginChild("tree", ImVec2(m_TreeWidth, 0), true))
		DrawFolder(m_Root);
	ImGui::EndChild();

	DrawSplitter(pane_h);

	if (ImGui::BeginChild("tiles", ImVec2(0, 0), true))
		DrawTiles();
	ImGui::EndChild();

	DrawCopyConfirm();

	ImGui::PopStyleVar(1);
	ImGui::End();
}
