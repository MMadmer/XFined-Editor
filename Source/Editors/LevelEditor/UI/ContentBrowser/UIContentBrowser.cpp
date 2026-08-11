#include "stdafx.h"
#include "..\..\..\XrECore\Editor\EditorGameContent.h"
#include "..\..\..\XrECore\Editor\EThumbnailVisual.h"
#include "..\..\..\XrECore\Editor\EDX11Utils.h"

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
static const SCategory kCategories[] =
{
	{ smObject,		"Objects",		true  },
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
		// project Content folder: plain disk scan, names are relative paths
		if (EditorProject::Active())
			ScanContentDir(EditorProject::Root(), "Content", m_Items);
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

	const bool is_model = IsVisualExt(name) ||
						  (m_Source == 1 && kCategories[m_Category].id == smVisual);
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
	// the order they are on screen", which only this pass knows
	m_DrawnOrder.clear();

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
			if (ImGui::Button("..", ImVec2(m_TileSize + 8.f, m_TileSize + 8.f)))
			{
				LPCSTR cut = strrchr(m_CurFolder.c_str(), '\\');
				if (cut)	m_CurFolder.erase(size_t(cut - m_CurFolder.c_str()));
				else		m_CurFolder.clear();
				ClearSelection();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("up one folder");
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

		ImTextureID tex	= GetThumb(full);
		const bool sel	= IsSelected(full);
		if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

		bool clicked;
		if (tex)	clicked = ImGui::ImageButton(tex, ImVec2(m_TileSize, m_TileSize));
		else		clicked = ImGui::Button(leaf, ImVec2(m_TileSize + 8.f, m_TileSize + 8.f));

		if (sel) ImGui::PopStyleColor();
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

	// clicking empty space clears the selection, as every file browser does.
	// IsWindowHovered + !IsAnyItemHovered keeps tiles and popups out of it.
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
		!ImGui::IsAnyItemHovered() &&
		ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		ClearSelection();

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

	if (IsGameSource())
	{
		if (ImGui::MenuItem(count > 1 ? "Copy selection to project" : "Copy to project"))
			CopySelection();
	}
	else if (m_Source == 1)
	{
		// Editor Content is read-only too, but its items are library refs
		// rather than files under a root the copier understands
		ImGui::MenuItem("Copy to project", "", false, false);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("not wired up yet for Editor Content");
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
	ImGui::MenuItem("Copy folder to project", "", false, false);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("recursive folder copy is not implemented yet - use \"Select contents\" and copy the items");

	if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(path);

	if (IsReadOnlySource())
	{
		ImGui::Separator();
		ImGui::TextDisabled("read-only source");
	}
	ImGui::EndPopup();
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
	ELog.Msg(mtInformation, "Copied %d item(s).", int(m_Clipboard.size()));
}

bool UIContentBrowser::CanPaste() const
{
	if (m_Clipboard.empty())		return false;
	if (m_Source != 0)				return false;	// destination must be writable
	if (m_ClipSource != 2)			return false;	// only the game copier exists so far
	return true;
}

LPCSTR UIContentBrowser::PasteBlockedReason() const
{
	if (m_Clipboard.empty())	return "clipboard is empty";
	if (m_Source != 0)			return "paste only into the project's Content - this source is read-only";
	if (m_ClipSource != 2)		return "copying out of Editor Content is not wired up yet";
	return "";
}

void UIContentBrowser::ClipboardPaste()
{
	if (!CanPaste()) return;

	// CopyToProject mirrors the item's own relative path under the project's
	// gamedata, so the paste lands where the engine expects to find it rather
	// than in whatever folder happens to be open.
	for (u32 i = 0; i < m_Clipboard.size(); ++i)
	{
		if (m_WantOverwrite) break;		// a conflict popup owns the rest
		RequestCopy(m_Clipboard[i].c_str());
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

void UIContentBrowser::CopySelection()
{
	if (!IsGameSource())
	{
		// Editor Content items are library references, not files with a path
		// under the game root, so the DARF copier cannot take them yet.
		ELog.DlgMsg(mtInformation,
			"Copying from Editor Content is not wired up yet - use DARF Content for now.");
		return;
	}
	if (m_Selection.empty())	return;

	// one confirmation per run: RequestCopy queues the first conflict and the
	// popup drives the rest on the next frames
	for (u32 i = 0; i < m_Selection.size(); ++i)
	{
		if (m_WantOverwrite)	break;
		RequestCopy(m_Selection[i].c_str());
	}
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
