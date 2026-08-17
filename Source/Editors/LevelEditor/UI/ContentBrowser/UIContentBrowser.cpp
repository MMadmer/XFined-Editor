#include "stdafx.h"
#include "..\..\..\XrECore\Editor\EditorGameContent.h"
#include "..\..\..\XrECore\Editor\EThumbnailVisual.h"
#include "..\..\..\XrECore\Editor\EDX11Utils.h"
#include "..\..\..\XrECore\Editor\EditorFileOps.h"
#include "..\..\..\XrECore\Editor\EditorModManifest.h"
#include "..\..\..\XrECore\Editor\Nq\NqExport.h"
#include "..\QuestGraph\UIQuestGraph.h"

// image formats the thumbnail path can decode directly
static bool IsImageExt(LPCSTR name)
{
	LPCSTR ext = name ? strrchr(name, '.') : 0;
	return ext && (!_stricmp(ext, ".dds") || !_stricmp(ext, ".tga") || !_stricmp(ext, ".png") ||
				   !_stricmp(ext, ".jpg") || !_stricmp(ext, ".bmp"));
}

// a scene file, wherever it is listed from
static bool IsLevelExt(LPCSTR name)
{
	LPCSTR ext = name ? strrchr(name, '.') : 0;
	return ext && !_stricmp(ext, ".level");
}

// models carry no baked thumbnail, so their preview has to be rendered
static bool IsVisualExt(LPCSTR name)
{
	LPCSTR ext = name ? strrchr(name, '.') : 0;
	return ext && !_stricmp(ext, ".ogf");
}

// the editor's own model format - engine visuals are .ogf, sources are .object
static bool IsObjectExt(LPCSTR name)
{
	LPCSTR ext = name ? strrchr(name, '.') : 0;
	return ext && !_stricmp(ext, ".object");
}

// NQ quest graph asset (docs/NQ_ARCHITECTURE.md)
static bool IsQuestExt(LPCSTR name)
{
	LPCSTR ext = name ? strrchr(name, '.') : 0;
	return ext && !_stricmp(ext, ".nqasset");
}

// Nothing under the project root is registered in the editor FS: a project may
// live anywhere (My Documents by default) and only $maps$ is remounted into it,
// so FS.r_open on a project file always fails. Project bytes come off the disk
// directly - the same reason images already load through DX11TextureFromFile.
static bool ReadProjectFile(LPCSTR rel, xr_vector<u8>& out)
{
	out.clear();
	if (!rel || !rel[0] || !EditorProject::Active()) return false;

	string_path abs;
	sprintf_s(abs, "%s\\%s", EditorProject::Root(), rel);
	HANDLE h = ::CreateFileA(abs, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
							 FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) return false;

	LARGE_INTEGER size;
	if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64 << 20))
	{
		::CloseHandle(h);
		return false;
	}
	out.resize(size_t(size.QuadPart));
	DWORD read = 0;
	const bool ok = !!::ReadFile(h, out.data(), DWORD(out.size()), &read, NULL) && read == out.size();
	::CloseHandle(h);
	if (!ok) out.clear();
	return ok;
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
xr_vector<UIContentBrowser::SLocation> UIContentBrowser::s_Favorites;
xr_string UIContentBrowser::s_FavoriteProject;
xr_string UIContentBrowser::s_FavoriteGameRoot;

// filled in by the preview window when it is compiled into the build
static UIContentBrowser::TShowVisual	s_ShowVisual	= 0;
static UIContentBrowser::TShowVisualMem	s_ShowVisualMem	= 0;
static UIContentBrowser::TShowVisualMem	s_ShowObjectMem	= 0;

void UIContentBrowser::SetVisualPreview(TShowVisual by_name, TShowVisualMem from_memory,
										TShowVisualMem object_from_memory)
{
	s_ShowVisual	= by_name;
	s_ShowVisualMem	= from_memory;
	s_ShowObjectMem	= object_from_memory;
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
static const u32 kNavigationHistoryLimit = 64;

static bool SameLocation(const UIContentBrowser::SLocation& lhs,
	const UIContentBrowser::SLocation& rhs)
{
	return lhs.source == rhs.source && (lhs.source != 1 || lhs.category == rhs.category) &&
		0 == _stricmp(lhs.folder.c_str(), rhs.folder.c_str());
}

static void RemoveScopedLocations(xr_vector<UIContentBrowser::SLocation>& locations,
	bool remove_project, bool remove_darf)
{
	for (u32 i = 0; i < locations.size();)
	{
		const int source = locations[i].source;
		if ((remove_project && source == 0) || (remove_darf && source == 2))
			locations.erase(locations.begin() + i);
		else
			++i;
	}
}

static bool NormalizeFolderPath(LPCSTR input, xr_string& out, xr_string& err)
{
	const xr_string raw = input ? input : "";
	out.clear();
	err.clear();
	if (raw.empty()) return true;

	xr_string part;
	for (LPCSTR p = raw.c_str();; ++p)
	{
		const char ch = *p;
		if (ch && ch != '\\' && ch != '/')
		{
			if (strchr(":*?\"<>|", ch))
			{
				err = "folder contains an invalid character";
				return false;
			}
			part += ch;
			continue;
		}

		if (!part.empty())
		{
			if (part == "." || part == "..")
			{
				err = "folder traversal is not allowed";
				return false;
			}
			if (!out.empty()) out += "\\";
			out += part;
			part.clear();
		}
		if (!ch) break;
	}

	if (out.size() >= sizeof(string_path))
	{
		err = "folder path is too long";
		return false;
	}
	return true;
}

static ImVec4 ThemeColor(XFinedTheme::ColorToken token, float alpha = 1.0f)
{
	const u32 rgb = XFinedTheme::Rgb(token);
	return ImVec4(float((rgb >> 16) & 0xff) / 255.0f,
		float((rgb >> 8) & 0xff) / 255.0f,
		float(rgb & 0xff) / 255.0f, alpha);
}

UIContentBrowser::UIContentBrowser()
{
	m_Category		= 0;
	// the project's own content is what someone opening the editor is working
	// on; the shared library is a place you go to fetch from
	m_Source		= 0;
	m_NeedRefresh	= true;
	m_TileSize		= 96.f;
	m_Tick			= 0;
	m_ClipSource		= -1;
	m_ClipCategory		= u32(-1);
	m_ClipCut			= false;
	m_HasAnchor			= false;
	m_HasPendingRange	= false;
	m_ScrollToSelection	= false;
	m_ConfirmDelete		= false;
	m_RenameBuf[0]		= 0;
	m_RenameFocus		= false;
	m_Marquee		= false;
	m_MarqueeStart	= ImVec2(0, 0);
	// preferences may not exist yet when the browser is constructed at startup
	if (CLevelPreferences* prefs = dynamic_cast<CLevelPreferences*>(EPrefs))
		m_TreeWidth	= float(prefs->ContentBrowserTreeWidth);
	else
		m_TreeWidth	= 220.f;
	m_Root.name		= "Content";
	m_Root.path		= "";
	m_DarfReady		= false;
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

bool UIContentBrowser::IsLevelsCategory(int index)
{
	return (index >= 0 && index < kCategoryCount) && (kLevelsCategoryId == kCategories[index].id);
}

int UIContentBrowser::FindCategory(LPCSTR name)
{
	if (!name || !name[0]) return -1;
	for (int i = 0; i < kCategoryCount; ++i)
		if (0 == _stricmp(kCategories[i].caption, name))
			return i;
	return -1;
}

UIContentBrowser::SLocation UIContentBrowser::CurrentLocation() const
{
	return SLocation(m_Source, int(m_Category), m_CurFolder.c_str());
}

void UIContentBrowser::PushHistory(xr_vector<SLocation>& history, const SLocation& location)
{
	if (!history.empty() && SameLocation(history.back(), location)) return;
	if (history.size() >= kNavigationHistoryLimit) history.erase(history.begin());
	history.push_back(location);
}

void UIContentBrowser::RememberNavigation(const SLocation& origin)
{
	if (SameLocation(origin, CurrentLocation())) return;
	PushHistory(m_BackHistory, origin);
	m_ForwardHistory.clear();
}

bool UIContentBrowser::ApplyLocation(SLocation location, bool record, xr_string& err)
{
	err.clear();
	if (location.source < 0) location.source = m_Source;
	if (location.category < 0) location.category = int(m_Category);
	if (location.source < 0 || location.source > 2)
	{
		err = "source must be 0 (project), 1 (editor), or 2 (DARF)";
		return false;
	}
	if (location.category < 0 || location.category >= kCategoryCount)
	{
		err = "asset category is out of range";
		return false;
	}
	if (!NormalizeFolderPath(location.folder.c_str(), location.folder, err)) return false;
	if (location.source == 0 && !EditorProject::Active())
	{
		err = "no project is open";
		return false;
	}

	EnsureListing();
	const SLocation origin = CurrentLocation();
	if (SameLocation(origin, location)) return true;

	const bool rebuild = location.source != m_Source || u32(location.category) != m_Category;
	if (rebuild)
	{
		m_Source = location.source;
		m_Category = u32(location.category);
		m_NeedRefresh = true;
		if (m_Source == 2)
		{
			m_DarfReady = false;
			m_DarfStatus = "mounting the linked game install...";
		}
		EnsureListing();
	}

	SFolder* folder = FindFolder(location.folder.c_str());
	if (!folder)
	{
		if (rebuild)
		{
			m_Source = origin.source;
			m_Category = u32(origin.category);
			m_NeedRefresh = true;
			Refresh();
			m_CurFolder = origin.folder;
		}
		err = "folder is not available in this source";
		return false;
	}

	location.folder = folder->path;
	if (record)
	{
		PushHistory(m_BackHistory, origin);
		m_ForwardHistory.clear();
	}
	m_CurFolder = location.folder;
	ClearSelection();
	m_Rename.clear();
	m_RenameFocus = false;
	m_ScrollToSelection = false;
	return true;
}

bool UIContentBrowser::GoBack(xr_string& err)
{
	EnsureListing();
	const SLocation current = CurrentLocation();
	xr_string last_error;
	while (!m_BackHistory.empty())
	{
		const SLocation target = m_BackHistory.back();
		if (ApplyLocation(target, false, err))
		{
			if (!m_BackHistory.empty() && SameLocation(m_BackHistory.back(), target))
				m_BackHistory.pop_back();
			PushHistory(m_ForwardHistory, current);
			return true;
		}
		last_error = err;
		if (!m_BackHistory.empty() && SameLocation(m_BackHistory.back(), target))
			m_BackHistory.pop_back();
	}
	err = last_error.empty() ? "back history is empty" : last_error;
	return false;
}

bool UIContentBrowser::GoForward(xr_string& err)
{
	EnsureListing();
	const SLocation current = CurrentLocation();
	xr_string last_error;
	while (!m_ForwardHistory.empty())
	{
		const SLocation target = m_ForwardHistory.back();
		if (ApplyLocation(target, false, err))
		{
			if (!m_ForwardHistory.empty() && SameLocation(m_ForwardHistory.back(), target))
				m_ForwardHistory.pop_back();
			PushHistory(m_BackHistory, current);
			return true;
		}
		last_error = err;
		if (!m_ForwardHistory.empty() && SameLocation(m_ForwardHistory.back(), target))
			m_ForwardHistory.pop_back();
	}
	err = last_error.empty() ? "forward history is empty" : last_error;
	return false;
}

bool UIContentBrowser::GoUp(xr_string& err)
{
	if (m_CurFolder.empty())
	{
		err = "already at the source root";
		return false;
	}

	xr_string parent = m_CurFolder;
	if (LPCSTR cut = strrchr(parent.c_str(), '\\'))
		parent.erase(size_t(cut - parent.c_str()));
	else
		parent.clear();
	return ApplyLocation(SLocation(m_Source, int(m_Category), parent.c_str()), true, err);
}

bool UIContentBrowser::GoHome(xr_string& err)
{
	if (m_CurFolder.empty())
	{
		err = "already at the source root";
		return false;
	}
	return ApplyLocation(SLocation(m_Source, int(m_Category), ""), true, err);
}

void UIContentBrowser::SyncFavoriteScopes()
{
	LPCSTR project_root = EditorProject::Active() ? EditorProject::Root() : "";
	LPCSTR game_root = EditorProject::GameRoot();
	const bool project_changed = 0 != _stricmp(s_FavoriteProject.c_str(), project_root);
	const bool game_changed = 0 != _stricmp(s_FavoriteGameRoot.c_str(), game_root);

	if (project_changed || game_changed)
		RemoveScopedLocations(s_Favorites, project_changed, project_changed || game_changed);

	s_FavoriteProject = project_root;
	s_FavoriteGameRoot = game_root;
}

void UIContentBrowser::GetHistory(SLocation& current, xr_vector<SLocation>& back,
	xr_vector<SLocation>& forward)
{
	SyncFavoriteScopes();
	if (Form)
	{
		Form->EnsureListing();
		current = Form->CurrentLocation();
		back = Form->m_BackHistory;
		forward = Form->m_ForwardHistory;
		return;
	}
	current = SLocation(-1, -1, "");
	back.clear();
	forward.clear();
}

bool UIContentBrowser::NavigateToFolder(int source, int category, LPCSTR folder, xr_string& err)
{
	Show();
	if (!Form)
	{
		err = "content browser unavailable";
		return false;
	}
	return Form->ApplyLocation(SLocation(source, category, folder), true, err);
}

bool UIContentBrowser::NavigateBack(xr_string& err)
{
	Show();
	if (!Form) { err = "content browser unavailable"; return false; }
	return Form->GoBack(err);
}

bool UIContentBrowser::NavigateForward(xr_string& err)
{
	Show();
	if (!Form) { err = "content browser unavailable"; return false; }
	return Form->GoForward(err);
}

bool UIContentBrowser::NavigateUp(xr_string& err)
{
	Show();
	if (!Form) { err = "content browser unavailable"; return false; }
	return Form->GoUp(err);
}

bool UIContentBrowser::NavigateHome(xr_string& err)
{
	Show();
	if (!Form) { err = "content browser unavailable"; return false; }
	return Form->GoHome(err);
}

int UIContentBrowser::FindFavorite(const SLocation& location) const
{
	for (u32 i = 0; i < s_Favorites.size(); ++i)
		if (SameLocation(s_Favorites[i], location)) return int(i);
	return -1;
}

bool UIContentBrowser::ValidateFavorite(SLocation& location, xr_string& err)
{
	if (location.source == m_Source &&
		(location.source != 1 || u32(location.category) == m_Category))
	{
		EnsureListing();
		SFolder* folder = FindFolder(location.folder.c_str());
		if (!folder)
		{
			err = "folder is not available in this source";
			return false;
		}
		location.folder = folder->path;
		return true;
	}

	if (location.source == 0)
	{
		if (!EditorProject::Active())
		{
			err = "no project is open";
			return false;
		}
		if (location.folder.empty()) return true;

		xr_string top = location.folder;
		if (const size_t cut = top.find('\\'); cut != xr_string::npos) top.erase(cut);
		if (EditorProject::IsEditorOnlyEntry(top.c_str()))
		{
			err = "folder is editor-owned, not project content";
			return false;
		}

		string_path absolute;
		const size_t absolute_length = xr_strlen(EditorProject::Root()) + 1 + location.folder.size();
		if (absolute_length >= sizeof(absolute))
		{
			err = "project favorite path is too long";
			return false;
		}
		xr_sprintf(absolute, sizeof(absolute), "%s\\%s", EditorProject::Root(), location.folder.c_str());
		const DWORD attributes = ::GetFileAttributesA(absolute);
		if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
		err = "folder is not available in this source";
		return false;
	}

	if (location.source == 2)
	{
		xr_string mount_error;
		if (!EditorGameContent::EnsureMounted(mount_error))
		{
			err = mount_error;
			return false;
		}
		if (location.folder.empty()) return true;

		const size_t length = location.folder.size();
		for (int i = 0; i < EditorGameContent::Count(); ++i)
		{
			LPCSTR path = EditorGameContent::Get(i)->path.c_str();
			if (xr_strlen(path) > length && 0 == _strnicmp(path, location.folder.c_str(), length) &&
				path[length] == '\\')
				return true;
		}
		err = "folder is not available in this source";
		return false;
	}

	if (location.folder.empty()) return true;
	ChooseItemVec items;
	if (kCategories[location.category].id == kLevelsCategoryId)
	{
		FS_FileSet levels;
		if (FS.file_list(levels, _maps_, FS_ListFiles | FS_ClampExt, "*.level"))
			for (FS_FileSetIt it = levels.begin(); it != levels.end(); ++it)
				items.push_back(SChooseItem(it->name.c_str(), ""));
		xr_vector<xr_string> base;
		EditorProject::ListBaseScenes(base);
		for (u32 i = 0; i < base.size(); ++i) items.push_back(SChooseItem(base[i].c_str(), ""));
	}
	else if (SChooseEvents* events = UIChooseForm::GetEvents(kCategories[location.category].id);
		events && events->on_fill)
	{
		events->on_fill(items, 0);
	}

	const size_t length = location.folder.size();
	for (u32 i = 0; i < items.size(); ++i)
	{
		LPCSTR path = items[i].name.c_str();
		if (xr_strlen(path) > length && 0 == _strnicmp(path, location.folder.c_str(), length) &&
			path[length] == '\\')
			return true;
	}
	err = "folder is not available in this source";
	return false;
}

void UIContentBrowser::GetFavorites(xr_vector<SLocation>& out)
{
	SyncFavoriteScopes();
	out = s_Favorites;
}

bool UIContentBrowser::AddFavorite(SLocation location, xr_string& err)
{
	Show();
	if (!Form) { err = "content browser unavailable"; return false; }
	Form->EnsureListing();
	if (location.source < 0) location.source = Form->m_Source;
	if (location.category < 0) location.category = int(Form->m_Category);
	if (location.source < 0 || location.source > 2)
	{
		err = "source must be 0 (project), 1 (editor), or 2 (DARF)";
		return false;
	}
	if (location.category < 0 || location.category >= kCategoryCount)
	{
		err = "asset category is out of range";
		return false;
	}
	if (!NormalizeFolderPath(location.folder.c_str(), location.folder, err)) return false;
	if (location.source == 0 && !EditorProject::Active())
	{
		err = "no project is open";
		return false;
	}
	if (Form->FindFavorite(location) >= 0)
	{
		err = "folder is already a favorite";
		return false;
	}

	if (!Form->ValidateFavorite(location, err)) return false;

	s_Favorites.push_back(location);
	err.clear();
	return true;
}

bool UIContentBrowser::RemoveFavorite(int index, xr_string& err)
{
	SyncFavoriteScopes();
	if (index < 0 || index >= int(s_Favorites.size()))
	{
		err = "favorite index is out of range";
		return false;
	}
	s_Favorites.erase(s_Favorites.begin() + index);
	err.clear();
	return true;
}

bool UIContentBrowser::NavigateFavorite(int index, xr_string& err)
{
	Show();
	SyncFavoriteScopes();
	if (Form) Form->EnsureListing();
	if (!Form || index < 0 || index >= int(s_Favorites.size()))
	{
		err = "favorite index is out of range";
		return false;
	}
	return Form->ApplyLocation(s_Favorites[index], true, err);
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
// `dirs` collects the folders themselves: an empty one has no items to derive it
// from, and a folder the user just created is empty by definition.
static bool ContentAbsolutePathFits(LPCSTR base, size_t relative_length)
{
	return xr_strlen(base) + 1 + relative_length < sizeof(string_path);
}

static void ScanContentDir(const char* base, const char* rel, ChooseItemVec& out,
						   xr_vector<xr_string>& dirs)
{
	char mask[MAX_PATH];
	const size_t base_length = xr_strlen(base);
	const size_t rel_length = xr_strlen(rel);
	if (base_length + 1 + rel_length + (rel_length ? 1 : 0) + 1 >= sizeof(mask)) return;
	sprintf_s(mask, "%s\\%s%s*", base, rel, rel[0] ? "\\" : "");
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		const size_t sub_length = rel_length + (rel_length ? 1 : 0) + xr_strlen(fd.cFileName);
		if (sub_length >= sizeof(string_path) || !ContentAbsolutePathFits(base, sub_length)) continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
		char sub[MAX_PATH];
		sprintf_s(sub, "%s%s%s", rel, rel[0] ? "\\" : "", fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			dirs.push_back(xr_string(sub));
			ScanContentDir(base, sub, out, dirs);
		}
		else
			out.push_back(SChooseItem(sub, ""));
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
}

void UIContentBrowser::EnsureListing()
{
	SyncFavoriteScopes();
	LPCSTR project_root = EditorProject::Active() ? EditorProject::Root() : "";
	LPCSTR game_root = EditorProject::GameRoot();
	const bool project_changed = 0 != _stricmp(m_ListedProject.c_str(), project_root);
	const bool game_changed = 0 != _stricmp(m_ListedGameRoot.c_str(), game_root);
	const bool invalidate_project = project_changed;
	const bool invalidate_darf = project_changed || game_changed;

	if (project_changed || game_changed)
	{
		m_ListedProject = project_root;
		m_ListedGameRoot = game_root;
		RemoveScopedLocations(m_BackHistory, invalidate_project, invalidate_darf);
		RemoveScopedLocations(m_ForwardHistory, invalidate_project, invalidate_darf);

		if (invalidate_darf)
		{
			m_DarfReady = false;
			m_DarfStatus = "mounting the linked game install...";
		}

		if ((m_Source == 0 && invalidate_project) || (m_Source == 2 && invalidate_darf))
		{
			m_NeedRefresh = true;
			m_CurFolder.clear();
			ClearSelection();
			m_Rename.clear();
			m_RenameFocus = false;
		}
	}
	if (m_NeedRefresh) Refresh();
}

void UIContentBrowser::Refresh()
{
	m_NeedRefresh	= false;
	m_Items.clear	();
	m_Dirs.clear	();
	DropCache		();

	if (m_Source == 0)
	{
		// The project ROOT is the content root. A module may keep any layout it
		// likes - mod.ltx [vfs] publishes it wherever the game needs it - so
		// every folder the author makes here is content by definition. Listing
		// only gamedata and rawdata hid the module's own parts (patch, spawn,
		// scripts, levels) and made the root look like a place you cannot work.
		if (EditorProject::Active())
		{
			WIN32_FIND_DATAA fd;
			string_path mask;
			HANDLE h = INVALID_HANDLE_VALUE;
			if (xr_strlen(EditorProject::Root()) + 2 < sizeof(mask))
			{
				sprintf_s(mask, "%s\\*", EditorProject::Root());
				h = ::FindFirstFileA(mask, &fd);
			}
			if (INVALID_HANDLE_VALUE != h)
			{
				do
				{
					if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
					// the editor's own scratch is not the author's content
					if (EditorProject::IsEditorOnlyEntry(fd.cFileName)) continue;
					if (!ContentAbsolutePathFits(EditorProject::Root(), xr_strlen(fd.cFileName))) continue;
					if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
					if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					{
						m_Dirs.push_back(xr_string(fd.cFileName));
						ScanContentDir(EditorProject::Root(), fd.cFileName, m_Items, m_Dirs);
					}
					else
						m_Items.push_back(SChooseItem(fd.cFileName, ""));
				} while (::FindNextFileA(h, &fd));
				::FindClose(h);
			}

			// The module's own machinery - the manifest and what export
			// generates - is not content and must not be deletable by a stray
			// Ctrl+A + Del. It is edited through Mod > Edit Manifest, and the
			// generated files follow the manifest on their own.
			xr_vector<xr_string> owned;
			EditorMod::GetOwnedFiles(EditorProject::Root(), owned);
			for (u32 k = 0; k < m_Items.size();)
			{
				bool hide = false;
				for (u32 o = 0; o < owned.size() && !hide; ++o)
					hide = (0 == _stricmp(m_Items[k].name.c_str(), owned[o].c_str()));
				if (hide)	m_Items.erase(m_Items.begin() + k);
				else		++k;
			}
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
		//
		// TWO sources, and the second one is the point: opening a project
		// remounts $maps$ onto that project, so the scenes the editor ships -
		// the sources every stock level was built from - vanish from the FS the
		// moment there is a project. This category is Editor Content: it lists
		// the library, whatever the project has mounted over it.
		m_Root.name = "Editor Content";
		FS_FileSet lst;
		if (FS.file_list(lst, _maps_, FS_ListFiles | FS_ClampExt, "*.level"))
		{
			FS_FileSetIt it = lst.begin(), e = lst.end();
			for (; it != e; ++it)
				m_Items.push_back(SChooseItem(it->name.c_str(), ""));
		}
		xr_vector<xr_string> base;
		EditorProject::ListBaseScenes(base);
		for (u32 i = 0; i < base.size(); ++i)
		{
			bool have = false;
			for (u32 k = 0; k < m_Items.size() && !have; ++k)
				have = (0 == _stricmp(m_Items[k].name.c_str(), base[i].c_str()));
			if (!have) m_Items.push_back(SChooseItem(base[i].c_str(), ""));
		}
	}
	else
	{
		SChooseEvents* E = UIChooseForm::GetEvents(kCategories[m_Category].id);
		if (E && E->on_fill) E->on_fill(m_Items, 0);
		m_Root.name = "Editor Content";
	}

	BuildTree		();
	if (!FindFolder(m_CurFolder.c_str()))
	{
		m_CurFolder.clear();
		ClearSelection();
	}
}

void UIContentBrowser::SwitchSource(int src)
{
	if (m_Source == src) return;
	xr_string err;
	ApplyLocation(SLocation(src, int(m_Category), ""), true, err);
}

UIContentBrowser::SFolder* UIContentBrowser::EnsureFolder(LPCSTR path)
{
	SFolder* cur = &m_Root;
	if (!path || !path[0]) return cur;

	xr_string acc;
	LPCSTR p = path;
	for (;;)
	{
		LPCSTR slash = strchr(p, '\\');
		// the tail after the last separator is a folder too when it came from
		// the directory list; BuildTree stops one short for item names
		const int len = slash ? int(slash - p) : int(xr_strlen(p));
		if (!len) break;

		string_path buf;
		int n = len;
		if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
		CopyMemory(buf, p, n);
		buf[n] = 0;

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
		if (!slash) break;
		p = slash + 1;
	}
	return cur;
}

void UIContentBrowser::BuildTree()
{
	m_Root.children.clear();
	m_Root.items.clear();

	// folders that exist on disk first, so an empty one still shows up
	for (u32 d = 0; d < m_Dirs.size(); ++d)
		EnsureFolder(m_Dirs[d].c_str());

	for (int i = 0; i < (int)m_Items.size(); ++i)
	{
		LPCSTR nm = m_Items[i].name.c_str();
		if (!nm || !nm[0]) continue;

		// everything up to the last separator is folders, the tail is the item
		LPCSTR slash = strrchr(nm, '\\');
		SFolder* cur = &m_Root;
		if (slash)
		{
			string_path dir;
			int len = int(slash - nm);
			if (len > (int)sizeof(dir) - 1) len = sizeof(dir) - 1;
			CopyMemory(dir, nm, len);
			dir[len] = 0;
			cur = EnsureFolder(dir);
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
		xr_vector<u8> bytes;
		if (ReadProjectFile(name, bytes))
		{
			// .object is the editor's source format and .ogf the engine's; both
			// render offscreen, but through different loaders
			if (IsObjectExt(name))	QueueObjectThumbnailFromMemory(name, bytes.data(), u32(bytes.size()));
			else					QueueVisualThumbnailFromMemory(name, bytes.data(), u32(bytes.size()));
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
	// .object counts here too: in the project source it is the usual way a model
	// arrives, and it has no baked .thm beside it the way the SDK library does
	const bool needs_render = IsVisualExt(name) || (m_Source == 0 && IsObjectExt(name));
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

	// Evict the coldest entry once the budget is blown - but NEVER one touched
	// this frame. A tile drawn this frame has its view recorded in an ImGui
	// draw list that has not been rendered yet; releasing it here hands the
	// driver a freed pointer at render time, which is exactly the flaky
	// igd10um64xe crash a folder bigger than the budget (prop: 759 textures vs
	// 512) produced. Strict '<' against m_Tick is the whole fix: a folder that
	// outgrows the budget simply keeps its tiles cached while it is on screen.
	if (m_Thumbs.size() >= kThumbBudget)
	{
		u32 oldest = m_Tick;
		ThumbMapIt victim = m_Thumbs.end();
		for (ThumbMapIt i = m_Thumbs.begin(); i != m_Thumbs.end(); ++i)
			if (i->second.last_used < oldest) { oldest = i->second.last_used; victim = i; }
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
// Unreal's scroll-into-view rules, used by both panes here.
//
// The first one is the one that matters: something already on screen is left
// EXACTLY where it is (SListView::ScrollIntoView - "Only scroll the item into
// view if it's not already in the visible range"). Recentring a tile the user
// can already see is what makes a browser feel like it ran away from the
// cursor. Past that, STileView centres a target that is far off, and merely
// pulls in one that is a row or so past an edge, leaving half a row of the
// next one visible (its NavigationScrollOffset, 0.5).
static void ScrollRectIntoView(float top_screen, float bottom_screen)
{
	const float view_top	= ImGui::GetScrollY();
	const float view_h		= ImGui::GetWindowHeight();
	const float win_y		= ImGui::GetWindowPos().y;
	// screen -> content space, which is what GetScrollY/SetScrollY speak
	const float top			= top_screen    - win_y + view_top;
	const float bottom		= bottom_screen - win_y + view_top;
	const float h			= bottom - top;

	if (top >= view_top && bottom <= view_top + view_h)	return;		// on screen

	if (top < view_top && top >= view_top - 2.f * h)
		ImGui::SetScrollY(top - h * 0.5f);					// just above
	else if (bottom > view_top + view_h && bottom <= view_top + view_h + 2.f * h)
		ImGui::SetScrollY(bottom - view_h + h * 0.5f);		// just below
	else
		ImGui::SetScrollY(top + h * 0.5f - view_h * 0.5f);	// far away: centre
}

// "rawdata" holds "rawdata\levels"; the root's empty path holds everything
static bool PathIsAncestor(const xr_string& parent, const xr_string& path)
{
	if (parent.empty())					return true;
	if (parent.size() > path.size())	return false;
	if (0 != _strnicmp(parent.c_str(), path.c_str(), parent.size()))	return false;
	return parent.size() == path.size() || path[parent.size()] == '\\';
}

void UIContentBrowser::DrawFolder(SFolder& f)
{
	const bool leaf	= f.children.empty();
	int flags		= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (leaf)					flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (m_CurFolder == f.path)	flags |= ImGuiTreeNodeFlags_Selected;
	if (&f == &m_Root)			flags |= ImGuiTreeNodeFlags_DefaultOpen;

	// a reveal drops the browser into a folder this tree may have collapsed -
	// open the chain down to it, so the pane agrees with the grid beside it
	if (m_ScrollToSelection && !leaf && PathIsAncestor(f.path, m_CurFolder))
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);

	const bool open = ImGui::TreeNodeEx(f.name.c_str(), flags);
	if (ImGui::IsItemClicked())
	{
		xr_string err;
		ApplyLocation(SLocation(m_Source, int(m_Category), f.path.c_str()), true, err);
		// A folder is an entry like any other: clicking it in the tree selects
		// it, so Delete / Copy / Rename act on it exactly as they do on a tile.
		// The root has no path of its own and cannot be one of them.
		if (!f.path.empty())	SelectEntry(f.path.c_str(), true, false, false);
		else					ClearSelection();
	}
	// the SAME menu the tiles get - a folder does not become a different thing
	// because it is drawn on the left
	if (!f.path.empty())	DrawEntryContextMenu(f.path.c_str(), true);
	if (m_ScrollToSelection && f.path == m_CurFolder)
		ScrollRectIntoView(ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().y);

	if (open && !leaf)
	{
		for (u32 i = 0; i < f.children.size(); ++i) DrawFolder(f.children[i]);
		ImGui::TreePop();
	}
}

// Turns whatever the Levels listing calls a level into a file COMMAND_LOAD can
// actually open. The listing clamps the extension off, and a level may be a
// bare "<name>.level" or a folder holding "<name>\<name>.level", so all the
// shapes are tried before giving up.
bool UIContentBrowser::ResolveLevelFile(LPCSTR name, int source, string_path& out)
{
	out[0] = 0;
	if (!name || !name[0])	return false;

	// the project's own Content names files relative to the project root
	if (0 == source && EditorProject::Active())
	{
		string_path abs;
		xr_sprintf	(abs, sizeof(abs), "%s\\%s", EditorProject::Root(), name);
		if (INVALID_FILE_ATTRIBUTES != ::GetFileAttributesA(abs))
		{
			xr_strcpy(out, sizeof(out), abs);
			return true;
		}
	}

	string_path stem;
	xr_strcpy	(stem, sizeof(stem), name);
	if (char* dot = strrchr(stem, '.'))
		if (!strchr(dot, '\\')) *dot = 0;

	LPCSTR leaf = strrchr(stem, '\\');
	leaf = leaf ? leaf + 1 : stem;

	string_path rel;
	// as listed, plus the extension
	xr_sprintf	(rel, sizeof(rel), "%s.level", stem);
	if (FS.exist(out, _maps_, rel))						return true;
	// a level folder: <stem>\<leaf>.level
	xr_sprintf	(rel, sizeof(rel), "%s\\%s.level", stem, leaf);
	if (FS.exist(out, _maps_, rel))						return true;

	// The game's own layout: levels\<name>\level.* is a COMPILED level - there
	// is no scene in there to open, the editor works on the source it was built
	// from. Map it to that source by name, which is what the author means by
	// "open l05_bar" while looking at the game's content.
	if (0 == _strnicmp(name, "levels\\", 7) || 0 == _strnicmp(name, "levels/", 7))
	{
		string_path lvl;
		xr_strcpy(lvl, sizeof(lvl), name + 7);
		for (char* p = lvl; *p; ++p)
			if (*p == '/') *p = '\\';
		if (char* cut = strchr(lvl, '\\')) *cut = 0;
		if (lvl[0])
		{
			xr_sprintf(rel, sizeof(rel), "%s.level", lvl);
			if (FS.exist(out, _maps_, rel))				return true;
			xr_sprintf(rel, sizeof(rel), "%s\\%s.level", lvl, lvl);
			if (FS.exist(out, _maps_, rel))				return true;
		}
	}

	// already a full path
	if (FS.exist(name))	{ xr_strcpy(out, sizeof(out), name); return true; }

	return false;
}

// Is this listing entry one of the game's levels? Everything under levels\ in
// the game source belongs to one, folder and compiled file alike.
bool UIContentBrowser::IsGameLevelEntry(LPCSTR name) const
{
	if (2 != m_Source || !name)	return false;
	if (0 != _strnicmp(name, "levels\\", 7) && 0 != _strnicmp(name, "levels/", 7)) return false;
	return name[7] != 0;
}

// Double-click action, Unreal-style: open the asset, never modify the scene.
// Models get the preview window; kinds with a dedicated editor go there; the
// rest report that they have no viewer yet instead of doing something random.
// `err` non-null means a caller wants the outcome back instead of a dialog -
// MCP drives this too, and a modal there would hang the editor on a machine
// nobody is sitting at.
bool UIContentBrowser::OpenAsset(LPCSTR name, xr_string* err)
{
#define OPEN_FAILED(...)	do {											\
		string256 _m; xr_sprintf(_m, sizeof(_m), __VA_ARGS__);			\
		if (err) *err = _m; else ELog.DlgMsg(mtError, "%s", _m);			\
		return false; } while(0)

	if (err) err->clear();
	if (!name || !name[0])	OPEN_FAILED("empty asset name");

	// A level IS the scene: opening one loads it, which is the one case where
	// the double-click legitimately changes what the editor is working on.
	// COMMAND_LOAD asks about unsaved changes itself.
	//
	// Both by extension and by category: the library lists levels by name with
	// the extension clamped off, while the project's own Content shows the
	// .level FILE - and a double click there has to do the same thing.
	const bool is_level = IsLevelExt(name) ||
						  (m_Source == 1 && kCategories[m_Category].id == kLevelsCategoryId) ||
						  IsGameLevelEntry(name);
	if (is_level)
	{
		// COMMAND_LOAD feeds its argument straight to FS.r_open and returns FALSE
		// without a word when that fails - and the listing hands out names with
		// the extension clamped off, which never opens. Resolve to a real file
		// first, and say so when there is none.
		string_path fn;
		if (!ResolveLevelFile(name, m_Source, fn))
		{
			// Not in the project, so it is the editor's own library copy: bring
			// it into the project and open THAT. The author edits their own
			// scene from the first click, and the shared library stays clean -
			// the same thing File > Import Base Scene does, without the detour.
			string_path scene;
			xr_strcpy(scene, sizeof(scene), name);
			for (char* p = scene; *p; ++p) if (*p == '/') *p = '\\';
			if (char* dot = strrchr(scene, '.'))
				if (!strchr(dot, '\\')) *dot = 0;
			// the game names a level by its folder: levels\l05_bar\...
			if (IsGameLevelEntry(scene))
			{
				xr_string tail = scene + 7;
				const size_t cut = tail.find('\\');
				xr_strcpy(scene, sizeof(scene), (cut == xr_string::npos ? tail : tail.substr(0, cut)).c_str());
			}

			xr_string import_err;
			if (EditorProject::HasBaseScene(scene) &&
				EditorProject::ImportBaseScene(scene, fn, import_err))
			{
				ELog.Msg(mtInformation, "Scene '%s' copied into the project - you are editing your own copy.", scene);
			}
			else if (IsGameLevelEntry(name))
			{
				// The game's levels are compiled: what the editor opens is the
				// source scene they were built from, and an install can ship a
				// level nobody has the source for.
				OPEN_FAILED("'%s' is a COMPILED level. The editor opens the SOURCE scene it was built\n"
							"from, and neither this project nor the editor library has one under that\n"
							"name.%s%s", name,
							import_err.empty() ? "" : "\n", import_err.c_str());
			}
			else
				OPEN_FAILED("no .level file behind '%s'", name);
		}

		ExecCommand(COMMAND_LOAD, xr_string(fn));
		return true;
	}

	// Library objects (.object) are previewable too - the preview window loads
	// them through CEditableObject when the model pool has nothing under that
	// name. Without this the most-used category in the browser opened nothing.
	// A project's own .object opens like a library one: it is the most common
	// thing in there, and it used to fall through to "no viewer for this kind".
	const bool is_model = IsVisualExt(name) || IsObjectExt(name) ||
						  (m_Source == 1 && (kCategories[m_Category].id == smVisual ||
											 kCategories[m_Category].id == smObject));
	if (is_model && (s_ShowVisual || s_ShowVisualMem || s_ShowObjectMem))
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
		else if (m_Source == 0)
		{
			// off the disk: nothing under the project root is in the editor FS
			xr_vector<u8> bytes;
			if (!ReadProjectFile(name, bytes))
				OPEN_FAILED("can't read '%s' from the project folder", name);
			const bool is_object = IsObjectExt(name);
			if (is_object && s_ShowObjectMem)		s_ShowObjectMem(name, bytes.data(), u32(bytes.size()));
			else if (!is_object && s_ShowVisualMem)	s_ShowVisualMem(name, bytes.data(), u32(bytes.size()));
			else									OPEN_FAILED("no viewer registered for '%s'", name);
		}
		else if (s_ShowVisual)
			s_ShowVisual(name);
		return true;
	}

	// Quest graphs open in their own tab (project files only: the graph editor
	// writes back through NqDoc, and the game/library sources are read-only)
	if (IsQuestExt(name))
	{
		if (m_Source != 0) OPEN_FAILED("%s: quest graphs open from the project only", name);
		xr_string q_err;
		if (UIQuestGraph::Open(name, &q_err)) return true;
		OPEN_FAILED("%s: %s", name, q_err.c_str());
	}

	// Textures get their own window, whatever source they came from. In the SDK
	// library the name carries no extension, so the category is what says it is
	// a texture; everywhere else the extension does.
	const bool is_texture = IsImageExt(name) ||
							(m_Source == 1 && (kCategories[m_Category].id == smTexture ||
											   kCategories[m_Category].id == smTextureRaw));
	if (is_texture)
	{
		xr_string img_err;
		if (UIImagePreview::Show(name, m_Source, &img_err))	return true;
		OPEN_FAILED("%s: %s", name, img_err.c_str());
	}

	OPEN_FAILED("no viewer for this asset kind yet: '%s'", name);
#undef OPEN_FAILED
}

// depth-first lookup by full path; null when the path is not in the tree
UIContentBrowser::SFolder* UIContentBrowser::FindFolder(LPCSTR path)
{
	if (!path || !path[0]) return &m_Root;

	xr_vector<SFolder*> stack; stack.push_back(&m_Root);
	while (!stack.empty())
	{
		SFolder* f = stack.back(); stack.pop_back();
		if (0 == _stricmp(f->path.c_str(), path)) return f;
		for (u32 i = 0; i < f->children.size(); ++i) stack.push_back(&f->children[i]);
	}
	return 0;
}

float UIContentBrowser::TileWidth() const
{
	return m_TileSize + ImGui::GetStyle().FramePadding.x * 2.f;
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

	// How many tiles fit on a row. A tile is WIDER than m_TileSize: a button
	// adds its frame padding on both sides, and the image tiles do the same, so
	// wrapping on the bare tile size put one column past the right edge every
	// time - clipped, unreachable, and it hid whatever was under it. N tiles
	// span N*(tile+spacing) - spacing, which is the row that has to fit.
	const ImGuiStyle&	st		= ImGui::GetStyle();
	const float			tile_w	= TileWidth();
	const float			space	= st.ItemSpacing.x;
	const float			avail	= ImGui::GetContentRegionAvail().x;
	int per_row					= (int)((avail + space) / (tile_w + space));
	if (per_row < 1) per_row = 1;

	// A new folder starts at its beginning. ImGui keeps the scroll offset per
	// child window, so without this, entering a folder from a scrolled grid
	// lands in the middle of the new one - it looks like the first rows simply
	// do not exist. Unreal resets the same way.
	if (m_ShownFolder != m_CurFolder)
	{
		m_ShownFolder = m_CurFolder;
		if (!m_ScrollToSelection) ImGui::SetScrollY(0.f);
	}

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
			ImGui::Button("..", ImVec2(tile_w, tile_w));
			// double click, same as every other folder tile - a single click
			// here navigated while clicking a folder did not, which is worse
			// than either rule on its own
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				xr_string err;
				GoUp(err);
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

			const bool renaming	= !m_Rename.empty() && (m_Rename == sub.path);
			const bool sel		= IsSelected(sub.path.c_str(), true);

			ImGui::PushID(1000000 + (int)c);
			ImGui::BeginGroup();

			// A folder tile is a grid entry like any other: it joins the drawn
			// order, so Shift-ranges and the rubber-band sweep over folders and
			// assets together instead of pretending they are different worlds.
			m_DrawnOrder.push_back(SEntry(sub.path.c_str(), true));
			const u32 folder_slot = u32(m_DrawnRects.size());
			m_DrawnRects.push_back(ImVec4(0, 0, 0, 0));

			// folders read as folders through colour: there is no icon atlas here.
			// Selected (or being renamed) is lit up.
			ImGui::PushStyleColor(ImGuiCol_Button,
				ThemeColor((sel || renaming) ? XFinedTheme::ColorToken::Accent
					: XFinedTheme::ColorToken::Input));
			ImGui::Button("[ ]", ImVec2(tile_w, tile_w));
			ImGui::PopStyleColor();

			m_DrawnRects[folder_slot] = ImVec4(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
											   ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);

			// One of the game's levels is a folder on disk, but it is a LEVEL to
			// the author: double-clicking it opens the scene it was built from
			// rather than showing them level.geom. The folder is still walkable
			// from the tree, so nothing becomes unreachable.
			string_path level_scene;
			const bool as_level = IsGameLevelEntry(sub.path.c_str()) &&
								  0 == strchr(sub.path.c_str() + 7, '\\') &&
								  ResolveLevelFile(sub.path.c_str(), m_Source, level_scene);

			// A single click SELECTS, a double click enters - exactly what an
			// asset tile does, so a folder can be part of a selection at all.
			if (ImGui::IsItemHovered())
			{
				const ImGuiIO& io = ImGui::GetIO();
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					SelectEntry(sub.path.c_str(), true, io.KeyCtrl, io.KeyShift);
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					if (as_level)	OpenAsset(sub.path.c_str());
					else
					{
						xr_string err;
						ApplyLocation(SLocation(m_Source, int(m_Category), sub.path.c_str()), true, err);
					}
				}
				if (!renaming)
					ImGui::SetTooltip(as_level ? "%s\nlevel - double click to open the scene it was built from"
											   : "%s", sub.path.c_str());
			}
			DrawEntryContextMenu(sub.path.c_str(), true);

			if (renaming)
			{
				ImGui::SetNextItemWidth(tile_w);
				if (m_RenameFocus)
				{
					// one frame late on purpose: the widget has to exist before
					// the keyboard can be handed to it
					ImGui::SetKeyboardFocusHere();
					m_RenameFocus = false;
				}
				// AutoSelectAll is the whole point - the name comes up fully
				// selected, so the first keystroke replaces it
				const bool done = ImGui::InputText("##cb_rename", m_RenameBuf, sizeof(m_RenameBuf),
					ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
				// Enter commits, and so does clicking away. Escape reverts the
				// buffer itself, so the commit sees an unchanged name and stops.
				if (done || ImGui::IsItemDeactivated()) CommitRename();
			}
			else
			{
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tile_w);
				ImGui::TextUnformatted(sub.name.c_str());
				ImGui::PopTextWrapPos();
			}
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
		m_DrawnOrder.push_back(SEntry(full, false));
		const u32 tile_slot = u32(m_DrawnRects.size());
		m_DrawnRects.push_back(ImVec4(0, 0, 0, 0));	// filled right after the widget

		ImTextureID tex		= GetThumb(full);
		const bool sel		= IsSelected(full, false);
		const bool renaming	= !m_Rename.empty() && (m_Rename == full);

		bool clicked;
		if (tex)	clicked = ImGui::ImageButton(tex, ImVec2(m_TileSize, m_TileSize));
		else		clicked = ImGui::Button(leaf, ImVec2(tile_w, tile_w));

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
			SelectEntry(full, false, io.KeyCtrl, io.KeyShift);
		}

		// Drag source for every source, not just the SDK library: dropping a
		// game or project asset on the viewport has to work the same way.
		// Dragging an unselected tile selects it first, as Unreal does, so the
		// payload always matches what is highlighted.
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			if (!IsSelected(full, false)) SelectEntry(full, false, false, false);
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
				SelectEntry(full, false, false, false);
				OpenAsset(full);
			}
		}

		// Right-click surface. Drawn last so the popup contents never become the
		// "last item" the hover check above reads.
		DrawEntryContextMenu(full, false);

		// caption under the tile, clipped to tile width - or the name box, which
		// an asset gets exactly like a folder does
		if (renaming)
		{
			ImGui::SetNextItemWidth(tile_w);
			if (m_RenameFocus)	{ ImGui::SetKeyboardFocusHere(); m_RenameFocus = false; }
			const bool done = ImGui::InputText("##cb_rename", m_RenameBuf, sizeof(m_RenameBuf),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
			if (done || ImGui::IsItemDeactivated()) CommitRename();
		}
		else
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tile_w);
			ImGui::TextUnformatted(leaf);
			ImGui::PopTextWrapPos();
		}

		ImGui::EndGroup();
		ImGui::PopID();
	}

	// Shift-click needed the whole grid order, so it is resolved here
	ApplyPendingRange();

	// A reveal brings the grid to what it selected: the LAST selected tile in
	// grid order, so a multi-item reveal lands on the end of the run instead of
	// jumping back to its start. The rects were captured as the tiles drew, and
	// a tile that is already on screen is left alone.
	if (m_ScrollToSelection)
		for (int i = _min(int(m_DrawnOrder.size()), int(m_DrawnRects.size())) - 1; i >= 0; --i)
			if (IsSelected(m_DrawnOrder[i].path.c_str(), m_DrawnOrder[i].folder))
			{
				ScrollRectIntoView(m_DrawnRects[i].y, m_DrawnRects[i].w);
				break;
			}

	// empty-space right-click; the shortcuts run from Draw() instead, so Del
	// and Ctrl+C reach the folder tree as well as the grid
	DrawGridContextMenu();

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
// Tile right-click. ONE menu for folders and assets alike: Copy, Cut, Paste and
// Delete mean the same thing whatever a tile holds, and a mixed selection is a
// perfectly ordinary selection. Only the entries that are genuinely about one
// kind - opening an asset's viewer, placing it in the scene - look at `folder`.
//
// Read-only contract: the SDK library and the linked game install can be copied
// OUT of and nothing else. Cut and Delete write, so they are refused there.
//------------------------------------------------------------------------------
void UIContentBrowser::DrawEntryContextMenu(LPCSTR path, bool folder)
{
	if (!ImGui::BeginPopupContextItem("cb_entry_ctx")) return;

	// right-clicking outside the selection moves it here, as Unreal does;
	// right-clicking inside keeps the multi-selection intact
	if (!IsSelected(path, folder)) SelectEntry(path, folder, false, false);

	const int count = int(m_Selection.size());
	if (count > 1)	ImGui::TextDisabled("%d selected", count);
	else			ImGui::TextDisabled("%s", path);
	ImGui::Separator();

	if (ImGui::MenuItem("Open"))
	{
		if (folder)
		{
			xr_string err;
			ApplyLocation(SLocation(m_Source, int(m_Category), path), true, err);
		}
		else		OpenAsset(path);
	}

	if (!folder)
	{
		string_path ref;
		const bool placeable = IsGameSource()
			? EditorGameContent::ResolvePlaceable(path, ref)
			: true;
		if (ImGui::MenuItem("Place in scene", "", false, placeable) && placeable)
			ExecCommand(COMMAND_CB_PLACE_ASSET,
						xr_string(IsGameSource() ? ref : path), u32(0));
		if (IsGameSource() && !placeable)
			ImGui::TextDisabled("  not a library object reference");
	}

	ImGui::Separator();

	// ---- the common commands ---------------------------------------------
	{
		int n = 0;
		// a library category with no file of its own (particles, light anims)
		// has nothing to hand over; everything else is copyable
		const bool copyable = (m_Source != 1) || !!LibFilesFor(kCategories[m_Category].id, n);
		if (ImGui::MenuItem("Copy", "Ctrl+C", false, copyable) && copyable)
			ClipboardCopy(false);
		if (!copyable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("this category lives inside one shared library file - there is no per-item file to copy");

		const bool writable = (m_Source == 0);
		if (ImGui::MenuItem("Cut", "Ctrl+X", false, writable) && writable)
			ClipboardCopy(true);
		if (!writable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("cut moves things, and this source is read-only");

		const bool can = CanPaste();
		if (ImGui::MenuItem("Paste", "Ctrl+V", false, can) && can) ClipboardPaste();
		if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", PasteBlockedReason());

		if (ImGui::MenuItem("Delete", "Del", false, writable) && writable)
			DeleteSelection();
		if (!writable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("this source is read-only");

		if (ImGui::MenuItem("Rename", "F2", false, writable && 1 == count) && writable)
			BeginRename(path);
	}

	ImGui::Separator();
	if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(path);

	if (IsReadOnlySource())
	{
		ImGui::Separator();
		ImGui::TextDisabled("read-only source");
	}
	ImGui::EndPopup();
}

bool UIContentBrowser::ResolveDropRef(LPCSTR name, string_path& ref)
{
	ref[0] = 0;
	if (!name || !name[0])	return false;
	const int source = Form ? Form->m_Source : 1;

	// the linked game install: only an .object the library can load qualifies
	if (2 == source)
		return EditorGameContent::ResolvePlaceable(name, ref);

	// the project: its rawdata\objects mirrors the library layout, so the ref
	// is the path below that root with the extension clamped off
	if (0 == source)
	{
		LPCSTR ext = strrchr(name, '.');
		if (!ext || 0 != _stricmp(ext, ".object"))	return false;

		LPCSTR rel = name;
		if (0 == _strnicmp(rel, "rawdata\\objects\\", 16))	rel += 16;
		else return false;

		xr_strcpy(ref, sizeof(string_path), rel);
		if (char* dot = strrchr(ref, '.'))	*dot = 0;
		return true;
	}

	// the SDK library: an Objects entry IS a reference; nothing else places
	if (Form && kCategories[Form->m_Category].id == smObject)
	{
		xr_strcpy(ref, sizeof(string_path), name);
		return true;
	}
	return false;
}

// The other placeable thing: a compiled engine model. A .ogf out of the
// project or the linked game (or a Visuals entry of the SDK library, which
// is the same thing by name) becomes a raw-visual scene object - the drop
// target passes the path through to COMMAND_CB_PLACE_ASSET with the visual
// bit set, and CSceneObject::SetVisual does the rest.
bool UIContentBrowser::ResolveDropVisual(LPCSTR name, string_path& path)
{
	path[0] = 0;
	if (!name || !name[0])	return false;
	const int source = Form ? Form->m_Source : 1;

	LPCSTR ext = strrchr(name, '.');
	if ((0 == source || 2 == source) && ext && 0 == _stricmp(ext, ".ogf"))
	{
		xr_strcpy(path, sizeof(string_path), name);
		return true;
	}
	if (1 == source && Form && kCategories[Form->m_Category].id == smVisual)
	{
		xr_strcpy(path, sizeof(string_path), name);
		return true;
	}
	return false;
}

//------------------------------------------------------------------------------
// MCP entry points
//------------------------------------------------------------------------------
bool UIContentBrowser::RevealAsset(LPCSTR name, int source, bool open_viewer, xr_string& err)
{
	if (!name || !name[0])	{ err = "empty asset name"; return false; }
	Show();									// the panel has to exist to show anything
	if (!Form)				{ err = "content browser unavailable"; return false; }
	const SLocation origin = Form->CurrentLocation();

	if (source >= 0 && source <= 2 && source != Form->m_Source)
		Form->SwitchSource(source);

	// the listing is rebuilt lazily; the lookup below needs it now
	Form->EnsureListing();

	int found = -1;
	for (u32 i = 0; i < Form->m_Items.size(); ++i)
		if (0 == _stricmp(Form->m_Items[i].name.c_str(), name)) { found = int(i); break; }

	// A folder is a legitimate thing to reveal - "show me levels\" is what a
	// caller browsing the tree actually wants, and there is no other way to
	// get the grid there without a mouse. Selected like a folder tile, so the
	// next frame scrolls the tree to it as well.
	if (found < 0)
	{
		string_path folder;
		strncpy_s(folder, sizeof(folder), name, _TRUNCATE);
		for (char* p = folder; *p; ++p) if (*p == '/') *p = '\\';
		if (const size_t n = xr_strlen(folder); n && folder[n - 1] == '\\') folder[n - 1] = 0;
		if (Form->FindFolder(folder))
		{
			xr_string nav_err;
			if (!Form->ApplyLocation(SLocation(Form->m_Source, int(Form->m_Category), folder), false, nav_err))
			{
				err = nav_err;
				return false;
			}
			Form->RememberNavigation(origin);
			Form->m_Filter.Clear();
			Form->m_Selection.clear();
			Form->m_Selection.push_back(SEntry(Form->m_CurFolder.c_str(), true));
			Form->m_Anchor		= Form->m_Selection.back();
			Form->m_HasAnchor	= true;
			Form->m_ScrollToSelection = true;
			// a game level is a folder on disk but a level to open
			if (open_viewer && Form->IsGameLevelEntry(Form->m_CurFolder.c_str()))
				return Form->OpenAsset(Form->m_CurFolder.c_str(), &err);
			return true;
		}
	}

	// The SDK library is split into categories, and a caller naming an asset has
	// no reason to know which one holds it - a level lives under Levels, a mesh
	// under Visuals. Sweep them rather than answering "not found" for something
	// that is plainly there.
	if (found < 0 && 1 == Form->m_Source)
	{
		const SLocation sweep_location = Form->CurrentLocation();
		const xr_vector<SEntry> sweep_selection = Form->m_Selection;
		const SEntry sweep_anchor = Form->m_Anchor;
		const SEntry sweep_pending_range = Form->m_PendingRange;
		const bool sweep_has_anchor = Form->m_HasAnchor;
		const bool sweep_has_pending_range = Form->m_HasPendingRange;
		const bool sweep_scroll_to_selection = Form->m_ScrollToSelection;
		for (int c = 0; c < kCategoryCount && found < 0; ++c)
		{
			if (c == sweep_location.category)	continue;
			Form->m_Category	= u32(c);
			Form->m_CurFolder	= "";
			Form->Refresh		();
			for (u32 i = 0; i < Form->m_Items.size(); ++i)
				if (0 == _stricmp(Form->m_Items[i].name.c_str(), name)) { found = int(i); break; }
		}
		if (found < 0)
		{
			Form->m_Source = sweep_location.source;
			Form->m_Category = u32(sweep_location.category);
			Form->m_CurFolder = sweep_location.folder;
			Form->Refresh();
			Form->m_Selection = sweep_selection;
			Form->m_Anchor = sweep_anchor;
			Form->m_PendingRange = sweep_pending_range;
			Form->m_HasAnchor = sweep_has_anchor;
			Form->m_HasPendingRange = sweep_has_pending_range;
			Form->m_ScrollToSelection = sweep_scroll_to_selection;
		}
	}

	if (found < 0)
	{
		err = "asset not found in this source";
		return false;
	}

	// navigate to the folder that holds it, clear any search that would hide it
	const xr_string found_name = Form->m_Items[found].name.c_str();
	LPCSTR	cut = strrchr(found_name.c_str(), '\\');
	const xr_string target_folder = cut ? found_name.substr(0, size_t(cut - found_name.c_str())) : xr_string("");
	xr_string nav_err;
	if (!Form->ApplyLocation(SLocation(Form->m_Source, int(Form->m_Category), target_folder.c_str()),
		false, nav_err))
	{
		err = nav_err;
		return false;
	}
	Form->RememberNavigation(origin);
	Form->m_Filter.Clear();

	Form->m_Selection.clear();
	Form->m_Selection.push_back(SEntry(found_name.c_str(), false));
	Form->m_Anchor		= Form->m_Selection.back();
	Form->m_HasAnchor	= true;
	// selecting it is only half a reveal - the next frame brings it into view
	Form->m_ScrollToSelection = true;

	// the reveal itself succeeded; a viewer that will not open is worth saying
	// out loud rather than reporting a bare ok
	if (open_viewer) return Form->OpenAsset(found_name.c_str(), &err);
	return true;
}

void UIContentBrowser::GetSelection(int& source, xr_string& folder, xr_vector<xr_string>& sel)
{
	source = Form ? Form->m_Source : -1;
	folder = Form ? Form->m_CurFolder : xr_string("");
	sel.clear();
	if (!Form) return;
	// folders come across with a trailing '\\', so a caller can still tell them
	// apart without the browser's own type leaking into the MCP surface
	for (u32 i = 0; i < Form->m_Selection.size(); ++i)
	{
		xr_string s(Form->m_Selection[i].path);
		if (Form->m_Selection[i].folder) s += "\\";
		sel.push_back(s);
	}
}

bool UIContentBrowser::McpCopyToProject(LPCSTR names, LPCSTR folder, LPCSTR dst,
										int source, int category,
										bool overwrite, int& files, xr_string& err)
{
	files	= 0;
	err		= "";

	if (!Form) Form = xr_new<UIContentBrowser>();

	const int target_source = source >= 0 ? source : Form->m_Source;
	const int target_category = category >= 0 ? category : int(Form->m_Category);
	if (target_category >= kCategoryCount)
	{
		err = "unknown category";
		return false;
	}
	if ((target_source != Form->m_Source || u32(target_category) != Form->m_Category) &&
		!Form->ApplyLocation(SLocation(target_source, target_category, ""), true, err))
		return false;
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

	// No destination: mirror into the layout fs.ltx implies - the useful default
	// for a script that just wants an asset where the engine will find it.
	if (!dst || !dst[0])
	{
		files = Form->CopyRefsToProject(kCategories[Form->m_Category].id, list, overwrite, err);
		Form->m_NeedRefresh = true;
		return err.empty();
	}

	// With one, go through the browser's own clipboard and paste, so a scripted
	// copy and a menu copy cannot drift apart.
	Form->m_Selection.clear();
	if (folder && folder[0])
	{
		// the FOLDER goes on the clipboard, not the items under it - that is what
		// makes the paste rebuild the tree instead of flattening it
		string_path norm;
		xr_strcpy	(norm, sizeof(norm), folder);
		for (char* p = norm; *p; ++p) if ('/' == *p) *p = '\\';
		Form->m_Selection.push_back(SEntry(norm, true));
	}
	else
		for (u32 i = 0; i < list.size(); ++i)
			Form->m_Selection.push_back(SEntry(list[i].c_str(), false));
	Form->ClipboardCopy(false);
	Form->ClearSelection();

	string_path dst_root;
	xr_string	mk_err;
	if (!EditorFileOps::MakeDir(dst, dst_root, mk_err))	{ err = mk_err; return false; }

	files = Form->PasteClipboardInto(dst_root, overwrite, err);
	return err.empty();
}

//------------------------------------------------------------------------------
// selection
//------------------------------------------------------------------------------
bool UIContentBrowser::IsSelected(LPCSTR path, bool folder) const
{
	const SEntry e(path, folder);
	for (u32 i = 0; i < m_Selection.size(); ++i)
		if (m_Selection[i] == e)	return true;
	return false;
}

void UIContentBrowser::ClearSelection()
{
	m_Selection.clear();
	m_HasAnchor			= false;
	m_HasPendingRange	= false;
}

void UIContentBrowser::SelectEntry(LPCSTR path, bool folder, bool additive, bool range)
{
	const SEntry e(path, folder);

	if (range && m_HasAnchor)
	{
		// resolved after the grid, where the drawn order is known
		m_PendingRange		= e;
		m_HasPendingRange	= true;
		return;
	}

	if (additive)
	{
		for (u32 i = 0; i < m_Selection.size(); ++i)
			if (m_Selection[i] == e)
			{
				m_Selection.erase(m_Selection.begin() + i);
				// the anchor must stay on something that is still selected
				if (m_Anchor == e)
				{
					m_HasAnchor = !m_Selection.empty();
					if (m_HasAnchor) m_Anchor = m_Selection.back();
				}
				return;
			}
		m_Selection.push_back(e);
		m_Anchor	= e;
		m_HasAnchor	= true;
		return;
	}

	m_Selection.clear();
	m_Selection.push_back(e);
	m_Anchor	= e;
	m_HasAnchor	= true;
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
			m_MarqueeBase	= io.KeyCtrl ? m_Selection : xr_vector<SEntry>();
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
			// plain AABB overlap: touched counts as selected. Folder tiles are in
			// this list too - the band does not care what a tile holds.
			if (r.z < a.x || r.x > b.x || r.w < a.y || r.y > b.y)	continue;
			if (!IsSelected(m_DrawnOrder[i].path.c_str(), m_DrawnOrder[i].folder))
				m_Selection.push_back(m_DrawnOrder[i]);
		}
		if (!m_Selection.empty()) { m_Anchor = m_Selection.back(); m_HasAnchor = true; }

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
	if (!m_HasPendingRange)	return;

	int from = -1, to = -1;
	for (u32 i = 0; i < m_DrawnOrder.size(); ++i)
	{
		if (m_DrawnOrder[i] == m_Anchor)		from = int(i);
		if (m_DrawnOrder[i] == m_PendingRange)	to	 = int(i);
	}
	m_HasPendingRange = false;
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
void UIContentBrowser::ClipExpandFolder(LPCSTR path)
{
	SFolder* f = FindFolder(path);
	if (!f)	return;

	LPCSTR leaf = strrchr(path, '\\');
	leaf = leaf ? leaf + 1 : path;
	// everything under the folder keeps its place BELOW the folder's own name,
	// so pasting it reproduces the tree rather than dumping it flat
	const size_t cut = xr_strlen(path);

	xr_vector<int> ids;
	CollectItems(*f, ids, true);
	for (u32 i = 0; i < ids.size(); ++i)
	{
		LPCSTR nm = m_Items[ids[i]].name.c_str();
		SClip c;
		c.src		= nm;
		c.folder	= false;
		c.rel		= leaf;
		// nm starts with path + '\\'
		if (xr_strlen(nm) > cut) { c.rel += "\\"; c.rel += (nm + cut + 1); }
		m_Clipboard.push_back(c);
	}
}

void UIContentBrowser::ClipboardCopy(bool cut)
{
	if (m_Selection.empty()) return;
	if (cut && m_Source != 0)
	{
		ELog.DlgMsg(mtInformation, "Cut moves things, and this source is read-only - use Copy.");
		return;
	}

	m_Clipboard.clear();
	m_ClipSource	= m_Source;
	m_ClipCategory	= kCategories[m_Category].id;
	m_ClipCut		= cut;

	for (u32 i = 0; i < m_Selection.size(); ++i)
	{
		const SEntry& e = m_Selection[i];
		LPCSTR leaf = strrchr(e.path.c_str(), '\\');
		leaf = leaf ? leaf + 1 : e.path.c_str();

		// A project folder goes across as a folder: the file copier walks it
		// itself. In a read-only source there is no folder on disk to walk, so
		// it is expanded into the items the listing knows about.
		if (e.folder && m_Source != 0)	{ ClipExpandFolder(e.path.c_str()); continue; }

		SClip c;
		c.src		= e.path;
		c.rel		= leaf;
		c.folder	= e.folder;
		m_Clipboard.push_back(c);
	}

	ELog.Msg(mtInformation, "%s %d entr%s.", cut ? "Cut" : "Copied",
			 int(m_Selection.size()), 1 == m_Selection.size() ? "y" : "ies");
}

// Arms the confirmation. Nothing is touched until DeleteConfirmed runs - a
// folder full of work used to go the moment Del was pressed, with no question
// asked and nothing to undo.
void UIContentBrowser::DeleteSelection()
{
	if (m_Selection.empty())	return;
	if (m_Source != 0)
	{
		ELog.DlgMsg(mtInformation, "This source is read-only - nothing here can be deleted.");
		return;
	}

	// name what is going, not just how much: "3 items" tells the user nothing
	// about whether the one they meant is among them
	int folders = 0, files = 0;
	for (u32 i = 0; i < m_Selection.size(); ++i)
		if (m_Selection[i].folder) ++folders; else ++files;

	char head[256];
	if (1 == m_Selection.size())
		sprintf_s(head, "Delete this %s?", m_Selection[0].folder ? "folder and everything in it" : "file");
	else if (folders && files)
		sprintf_s(head, "Delete %d folder(s) (with everything in them) and %d file(s)?", folders, files);
	else if (folders)
		sprintf_s(head, "Delete %d folder(s) and everything in them?", folders);
	else
		sprintf_s(head, "Delete %d file(s)?", files);

	m_ConfirmText = head;
	m_ConfirmText += "\n\n";
	const u32 shown = _min(u32(8), u32(m_Selection.size()));
	for (u32 i = 0; i < shown; ++i)
	{
		m_ConfirmText += "    ";
		m_ConfirmText += m_Selection[i].path;
		if (m_Selection[i].folder) m_ConfirmText += "\\";
		m_ConfirmText += "\n";
	}
	if (m_Selection.size() > shown)
	{
		char more[64];
		sprintf_s(more, "    ... and %d more\n", int(m_Selection.size() - shown));
		m_ConfirmText += more;
	}
	m_ConfirmDelete = true;
}

void UIContentBrowser::DeleteConfirmed()
{
	if (m_Selection.empty() || m_Source != 0)	return;

	EditorFileOps::SReport rep;
	for (u32 i = 0; i < m_Selection.size(); ++i)
	{
		string_path abs;
		xr_sprintf	(abs, sizeof(abs), "%s\\%s", EditorProject::Root(), m_Selection[i].path.c_str());
		// recursive: the selection said "this", not "this if it happens to be empty"
		EditorFileOps::Delete(abs, true, rep);
	}

	ClearSelection();
	m_NeedRefresh = true;
	if (rep.ok())	ELog.Msg(mtInformation, "Deleted %d file(s), %d folder(s) - they are in the Recycle Bin.",
							 rep.files, rep.dirs);
	else			ELog.DlgMsg(mtError, "Deleted %d file(s); %d failed - %s", rep.files, rep.failed,
								rep.failures.empty() ? "" : rep.failures[0].reason.c_str());
}

void UIContentBrowser::DrawConfirmDelete()
{
	if (!ImGui::BeginPopupModal("Delete", 0, ImGuiWindowFlags_AlwaysAutoResize)) return;

	ImGui::TextUnformatted(m_ConfirmText.c_str());
	ImGui::Separator();
	ImGui::TextDisabled("Goes to the Recycle Bin - restorable from there.");
	ImGui::Spacing();

	// Cancel is the default: Enter and Escape both back out, so a stray keypress
	// on a just-opened dialog can never delete anything
	const bool del = ImGui::Button("Delete", ImVec2(110, 0));
	ImGui::SameLine();
	const bool cancel = ImGui::Button("Cancel", ImVec2(110, 0)) ||
						ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
						ImGui::IsKeyPressed(ImGuiKey_Enter, false);

	if (del)		DeleteConfirmed();
	if (del || cancel)
	{
		m_ConfirmDelete = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

bool UIContentBrowser::CanPaste() const
{
	if (m_Clipboard.empty())		return false;
	if (m_Source != 0)				return false;	// destination must be writable
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
	if (m_Source != 0)			return "paste into the project's Content - this source is read-only";
	if (m_ClipSource == 1)
	{
		int n = 0;
		if (!LibFilesFor(m_ClipCategory, n))
			return "that category lives inside one shared library file - there is no per-item file to copy";
	}
	return "";
}

void UIContentBrowser::PasteEntry(const SClip& c, LPCSTR dst_root, bool overwrite,
								  EditorFileOps::SReport& rep)
{
	// `rel` may carry folders of its own (a copied tree); the entry itself keeps
	// its leaf name, so the destination FOLDER is rel minus that leaf
	string_path dst;
	xr_strcpy	(dst, sizeof(dst), dst_root);
	{
		xr_string sub(c.rel);
		if (LPCSTR cut = strrchr(sub.c_str(), '\\'))
		{
			sub.erase(size_t(cut - sub.c_str()));
			xr_strcat(dst, sizeof(dst), "\\");
			xr_strcat(dst, sizeof(dst), sub.c_str());
		}
	}

	switch (m_ClipSource)
	{
	case 0:
		{
			// project -> project: real paths on both sides, and Copy/Move walk a
			// folder themselves - which is why project folders are not expanded
			string_path src;
			xr_sprintf	(src, sizeof(src), "%s\\%s", EditorProject::Root(), c.src.c_str());
			if (m_ClipCut)	EditorFileOps::Move(src, dst, true, overwrite, rep);
			else			EditorFileOps::Copy(src, dst, true, overwrite, rep);
		}
		break;

	case 2:
		// the linked game install: a gamedata-relative path that may live inside
		// an archive, which EditorFileOps::Copy knows how to read
		EditorFileOps::Copy(c.src.c_str(), dst, false, overwrite, rep);
		break;

	default:
		{
			// the SDK library: a reference, resolved through the fs.ltx aliases
			xr_string err;
			const int n = CopyLibraryItem(m_ClipCategory, c.src.c_str(), dst, overwrite, err);
			if (!err.empty())
			{
				++rep.failed;
				if (rep.failures.size() < 64)
				{
					EditorFileOps::SFailure f;
					f.path		= c.src;
					f.reason	= err;
					rep.failures.push_back(f);
				}
			}
			rep.files += n;
		}
		break;
	}
}

int UIContentBrowser::PasteClipboardInto(LPCSTR dst_root, bool overwrite, xr_string& err)
{
	err = "";

	EditorFileOps::SReport rep;
	for (u32 i = 0; i < m_Clipboard.size(); ++i)
		PasteEntry(m_Clipboard[i], dst_root, overwrite, rep);

	// a cut is spent once it has been pasted, like everywhere else
	if (m_ClipCut)	{ m_Clipboard.clear(); m_ClipCut = false; }

	m_NeedRefresh = true;
	if (!rep.ok())
	{
		char head[96];
		sprintf_s(head, sizeof(head), "%d failed, first: ", rep.failed);
		err  = head;
		err += rep.failures.empty() ? "" : rep.failures[0].reason.c_str();
	}
	return rep.files;
}

void UIContentBrowser::ClipboardPaste()
{
	if (!CanPaste()) return;

	// Straight into the folder you are looking at - that is what a paste means.
	string_path dst_root;
	xr_sprintf	(dst_root, sizeof(dst_root), "%s\\%s", EditorProject::Root(), m_CurFolder.c_str());

	// A paste has always overwritten whatever was in the way, without a word.
	// The clipboard entries carry the path they will take below dst_root, so
	// the collisions are known BEFORE anything is written - ask about those.
	int clashes = 0;
	xr_string first_clash;
	for (u32 i = 0; i < m_Clipboard.size(); ++i)
	{
		string_path t;
		xr_sprintf(t, sizeof(t), "%s\\%s", dst_root, m_Clipboard[i].rel.c_str());
		if (INVALID_FILE_ATTRIBUTES == ::GetFileAttributesA(t)) continue;
		if (!clashes) first_clash = m_Clipboard[i].rel;
		++clashes;
	}
	if (clashes)
	{
		// -nodlg answers No, so an unattended run never silently replaces files
		if (mrYes != ELog.DlgMsg(mtConfirmation, mbYes | mbNo,
			"%d item(s) already exist in '%s' and will be REPLACED (first: %s).\n\nOverwrite?",
			clashes, m_CurFolder.empty() ? "this folder" : m_CurFolder.c_str(), first_clash.c_str()))
			return;
	}

	xr_string err;
	const int files = PasteClipboardInto(dst_root, true, err);
	if (err.empty())	ELog.Msg(mtInformation, "Pasted %d file(s).", files);
	else				ELog.DlgMsg(mtError, "Pasted %d file(s); %s", files, err.c_str());
}

void UIContentBrowser::HandleShortcuts()
{
	// only when this panel owns the keyboard, or Ctrl+C in the viewport would
	// end up copying assets
	if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
	if (UI) UI->BlockShortCuts();
	if (ImGui::IsAnyItemActive()) return;

	const ImGuiIO& io = ImGui::GetIO();
	if (io.KeyAlt)
	{
		xr_string err;
		if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) GoBack(err);
		if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) GoForward(err);
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) GoUp(err);
		if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) GoHome(err);
	}

	// Delete carries no modifier, and F2 renames whatever single entry is
	// selected - both belong outside the Ctrl gate below
	if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))	DeleteSelection();
	if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && 1 == m_Selection.size())
		BeginRename(m_Selection[0].path.c_str());

	if (!io.KeyCtrl) return;

	if (ImGui::IsKeyPressed(ImGuiKey_C, false))	ClipboardCopy(false);
	if (ImGui::IsKeyPressed(ImGuiKey_X, false))	ClipboardCopy(true);
	if (ImGui::IsKeyPressed(ImGuiKey_V, false))	ClipboardPaste();
	if (ImGui::IsKeyPressed(ImGuiKey_A, false))
	{
		m_Selection.clear();
		for (u32 i = 0; i < m_DrawnOrder.size(); ++i)
			m_Selection.push_back(m_DrawnOrder[i]);
		if (!m_Selection.empty()) { m_Anchor = m_Selection.back(); m_HasAnchor = true; }
	}
}

void UIContentBrowser::DrawGridContextMenu()
{
	// "Window" variant: fires on empty space, not over a tile
	if (!ImGui::BeginPopupContextWindow("cb_grid_ctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		return;

	// Authoring lives here, on the empty space of the writable source - the same
	// place a file manager puts it.
	if (m_Source == 0)
	{
		// the root included: a module's layout is its author's business
		if (ImGui::MenuItem("New folder"))	CreateFolder();
		if (ImGui::BeginMenu("Create"))
		{
			if (ImGui::MenuItem("Quest Graph"))	CreateQuest();
			ImGui::EndMenu();
		}
		ImGui::Separator();
	}

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
		// everything on screen, folders included - one selection, one meaning
		m_Selection.clear();
		for (u32 i = 0; i < m_DrawnOrder.size(); ++i)
			m_Selection.push_back(m_DrawnOrder[i]);
		if (!m_Selection.empty()) { m_Anchor = m_Selection.back(); m_HasAnchor = true; }
	}
	if (ImGui::MenuItem("Clear selection", "", false, !m_Selection.empty()))
		ClearSelection();
	if (ImGui::MenuItem("Refresh")) m_NeedRefresh = true;

	ImGui::EndPopup();
}

// `dst_dir` empty means "wherever fs.ltx implies", which is what MCP wants when
// no destination was given; a paste passes the folder the user is looking at.
int UIContentBrowser::CopyLibraryItem(u32 category, LPCSTR ref, LPCSTR dst_dir,
									  bool overwrite, xr_string& err)
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

		if (dst_dir && dst_dir[0])
		{
			string_path src;
			FS.update_path	(src, files[i].alias, rel);
			if (INVALID_FILE_ATTRIBUTES == ::GetFileAttributesA(src))	continue;
			++hits;
			EditorFileOps::Copy(src, dst_dir, false, overwrite, rep);
		}
		else if (EditorFileOps::CopyLibraryFile(files[i].alias, rel, overwrite, rep))
			++hits;
	}

	if (!hits)									err = "no file behind this reference";
	else if (!rep.ok() && !rep.failures.empty())	err = rep.failures[0].reason;
	return rep.files;
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
			// no destination: mirror into the layout fs.ltx implies
			files += CopyLibraryItem(category, names[i].c_str(), 0, overwrite, one);
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

//------------------------------------------------------------------------------
// folder authoring (project source only)
//------------------------------------------------------------------------------
void UIContentBrowser::CreateFolder()
{
	if (m_Source != 0 || !EditorProject::Active())	return;

	// The project root is a content folder like any other: a module keeps
	// whatever layout its author wants ([vfs] publishes it), so a folder can be
	// born anywhere - including next to gamedata.

	// first free "New Folder", "New Folder 2", ... exactly like a file manager
	xr_string rel;
	string_path probe;
	for (int n = 1; n <= 999; ++n)
	{
		char leaf[64];
		if (1 == n)	xr_strcpy(leaf, sizeof(leaf), "New Folder");
		else		sprintf_s(leaf, "New Folder %d", n);

		if (m_CurFolder.empty())	sprintf_s(probe, "%s\\%s", EditorProject::Root(), leaf);
		else						sprintf_s(probe, "%s\\%s\\%s", EditorProject::Root(), m_CurFolder.c_str(), leaf);
		if (INVALID_FILE_ATTRIBUTES == ::GetFileAttributesA(probe))
		{
			rel = m_CurFolder;
			if (!rel.empty()) rel += "\\";
			rel += leaf;
			break;
		}
	}
	if (rel.empty())	{ ELog.DlgMsg(mtError, "Too many unnamed folders here."); return; }

	string_path full;
	xr_string err;
	if (!EditorFileOps::MakeDir(rel.c_str(), full, err))
	{
		ELog.DlgMsg(mtError, "Cannot create the folder: %s", err.c_str());
		return;
	}

	// the tile has to exist before it can be renamed, and only a refresh puts it
	// in the tree - an empty folder has no items to derive it from
	Refresh			();
	ClearSelection	();
	BeginRename		(rel.c_str());
}

void UIContentBrowser::CreateQuest()
{
	if (m_Source != 0 || !EditorProject::Active())	return;

	// first free NewQuest.nqasset, NewQuest2.nqasset, ... in the open folder
	xr_string rel;
	string_path probe;
	for (int n = 1; n <= 999; ++n)
	{
		char leaf[64];
		if (1 == n)	xr_strcpy(leaf, sizeof(leaf), "NewQuest.nqasset");
		else		sprintf_s(leaf, "NewQuest%d.nqasset", n);
		if (m_CurFolder.empty())	sprintf_s(probe, "%s\\%s", EditorProject::Root(), leaf);
		else						sprintf_s(probe, "%s\\%s\\%s", EditorProject::Root(), m_CurFolder.c_str(), leaf);
		if (INVALID_FILE_ATTRIBUTES == ::GetFileAttributesA(probe))
		{
			rel = m_CurFolder;
			if (!rel.empty()) rel += "\\";
			rel += leaf;
			break;
		}
	}
	if (rel.empty())	{ ELog.DlgMsg(mtError, "Too many unnamed quests here."); return; }

	xr_string err;
	if (!NqExport::NewQuestTemplate(probe, err))
	{
		ELog.DlgMsg(mtError, "Cannot create the quest: %s", err.c_str());
		return;
	}
	Refresh			();
	ClearSelection	();
	BeginRename		(rel.c_str());
}

void UIContentBrowser::BeginRename(LPCSTR path)
{
	if (m_Source != 0 || !path || !path[0])	return;

	m_Rename		= path;
	m_RenameFocus	= true;

	LPCSTR leaf = strrchr(path, '\\');
	xr_strcpy(m_RenameBuf, sizeof(m_RenameBuf), leaf ? leaf + 1 : path);
}

void UIContentBrowser::CommitRename()
{
	if (m_Rename.empty())	return;

	const xr_string was = m_Rename;
	m_Rename.clear();

	// trim, then bail on anything that is not a plain leaf name
	char leaf[128];
	xr_strcpy(leaf, sizeof(leaf), m_RenameBuf);
	char* s = leaf;
	while (' ' == *s) ++s;
	for (char* e = s + xr_strlen(s); e > s && ' ' == e[-1];) *--e = 0;

	LPCSTR old_leaf = strrchr(was.c_str(), '\\');
	old_leaf = old_leaf ? old_leaf + 1 : was.c_str();
	if (!s[0] || 0 == xr_strcmp(s, old_leaf))	return;		// unchanged, nothing to do

	if (strpbrk(s, "\\/:*?\"<>|"))
	{
		ELog.DlgMsg(mtError, "A name cannot contain \\ / : * ? \" < > |");
		return;
	}
	// a quest renamed without an extension keeps being a quest; putting the
	// extension back can land on the old name again ("Quest.nqasset" -> "Quest"),
	// which is not a rename at all
	xr_string keep_ext;
	if (IsQuestExt(old_leaf) && !strrchr(s, '.'))
	{
		keep_ext = xr_string(s) + ".nqasset";
		s = const_cast<char*>(keep_ext.c_str());
		if (0 == xr_strcmp(s, old_leaf)) return;
	}

	xr_string parent(was);
	if (LPCSTR cut = strrchr(parent.c_str(), '\\'))
		parent.erase(size_t(cut - parent.c_str()));
	else
		parent.clear();

	string_path from, to;
	sprintf_s(from, "%s\\%s", EditorProject::Root(), was.c_str());
	if (parent.empty())	sprintf_s(to, "%s\\%s", EditorProject::Root(), s);
	else				sprintf_s(to, "%s\\%s\\%s", EditorProject::Root(), parent.c_str(), s);

	if (INVALID_FILE_ATTRIBUTES != ::GetFileAttributesA(to))
	{
		ELog.DlgMsg(mtError, "'%s' is already there.", s);
		return;
	}
	if (!::MoveFileA(from, to))
	{
		ELog.DlgMsg(mtError, "Cannot rename '%s' (error %d).", old_leaf, int(::GetLastError()));
		return;
	}

	// keep the user where they were looking: the open folder may BE the renamed
	// one, or sit under it
	xr_string now(parent);
	if (!now.empty()) now += "\\";
	now += s;
	if (PathIsAncestor(was, m_CurFolder))
		m_CurFolder = now + (m_CurFolder.c_str() + was.size());

	auto rewrite_location = [&](SLocation& location)
	{
		if (location.source != 0 || !PathIsAncestor(was, location.folder)) return;
		location.folder = now + (location.folder.c_str() + was.size());
	};
	for (u32 i = 0; i < m_BackHistory.size(); ++i) rewrite_location(m_BackHistory[i]);
	for (u32 i = 0; i < m_ForwardHistory.size(); ++i) rewrite_location(m_ForwardHistory[i]);
	for (u32 i = 0; i < s_Favorites.size(); ++i) rewrite_location(s_Favorites[i]);

	m_NeedRefresh = true;
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

static LPCSTR BrowserSourceName(int source)
{
	switch (source)
	{
	case 0: return "Content";
	case 1: return "Editor Content";
	case 2: return "DARF Content";
	default: return "Unknown";
	}
}

void UIContentBrowser::DrawBreadcrumbs()
{
	xr_string err;
	xr_string requested;
	bool navigate = false;
	const bool at_root = m_CurFolder.empty();
	if (at_root) ImGui::PushStyleColor(ImGuiCol_Button, ThemeColor(XFinedTheme::ColorToken::Accent));
	if (ImGui::SmallButton(BrowserSourceName(m_Source))) navigate = true;
	if (at_root) ImGui::PopStyleColor();

	xr_string path;
	LPCSTR begin = m_CurFolder.c_str();
	while (begin && begin[0])
	{
		LPCSTR cut = strchr(begin, '\\');
		const size_t length = cut ? size_t(cut - begin) : xr_strlen(begin);
		xr_string part;
		part.assign(begin, length);
		if (!path.empty()) path += "\\";
		path += part;

		ImGui::SameLine(0.0f, 5.0f);
		ImGui::TextDisabled(">");
		ImGui::SameLine(0.0f, 5.0f);
		ImGui::PushID(path.c_str());
		const bool current = 0 == _stricmp(path.c_str(), m_CurFolder.c_str());
		if (current) ImGui::PushStyleColor(ImGuiCol_Button, ThemeColor(XFinedTheme::ColorToken::Accent));
		if (ImGui::SmallButton(part.c_str()))
		{
			requested = path;
			navigate = true;
		}
		if (current) ImGui::PopStyleColor();
		ImGui::PopID();
		if (!cut) break;
		begin = cut + 1;
	}
	if (navigate)
		ApplyLocation(SLocation(m_Source, int(m_Category), requested.c_str()), true, err);
}

void UIContentBrowser::DrawFavoritesPopup()
{
	if (!ImGui::BeginPopup("cb_favorites")) return;

	ImGui::TextColored(ThemeColor(XFinedTheme::ColorToken::Accent), "Favorite folders");
	ImGui::Separator();
	if (s_Favorites.empty())
		ImGui::TextDisabled("No favorites in this session");

	for (u32 i = 0; i < s_Favorites.size(); ++i)
	{
		const SLocation favorite = s_Favorites[i];
		xr_string label = BrowserSourceName(favorite.source);
		if (favorite.source == 1)
		{
			label += " / ";
			label += CategoryName(favorite.category);
		}
		if (!favorite.folder.empty())
		{
			label += " / ";
			label += favorite.folder;
		}

		ImGui::PushID(int(i));
		const float remove_width = ImGui::GetFrameHeight();
		const float row_width = ImGui::GetContentRegionAvail().x - remove_width - ImGui::GetStyle().ItemSpacing.x;
		if (ImGui::Selectable(label.c_str(), SameLocation(favorite, CurrentLocation()),
			ImGuiSelectableFlags_None, ImVec2(row_width > 80.0f ? row_width : 80.0f, 0.0f)))
		{
			xr_string err;
			if (!ApplyLocation(favorite, true, err))
				ELog.Msg(mtError, "Cannot open favorite: %s", err.c_str());
			else
				ImGui::CloseCurrentPopup();
			ImGui::PopID();
			break;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("x"))
		{
			s_Favorites.erase(s_Favorites.begin() + i);
			ImGui::PopID();
			break;
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove favorite");
		ImGui::PopID();
	}

	ImGui::EndPopup();
}

void UIContentBrowser::DrawNavigationBar()
{
	ImGui::PushID("cb_navigation");
	xr_string err;
	ImGui::BeginDisabled(m_BackHistory.empty());
	if (ImGui::SmallButton("<")) GoBack(err);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Back (Alt+Left)");

	ImGui::SameLine();
	ImGui::BeginDisabled(m_ForwardHistory.empty());
	if (ImGui::SmallButton(">")) GoForward(err);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Forward (Alt+Right)");

	ImGui::SameLine();
	ImGui::BeginDisabled(m_CurFolder.empty());
	if (ImGui::SmallButton("Up")) GoUp(err);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Parent folder (Alt+Up)");

	ImGui::SameLine();
	ImGui::BeginDisabled(m_CurFolder.empty());
	if (ImGui::SmallButton("Home")) GoHome(err);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Source root (Alt+Home)");

	const int favorite = FindFavorite(CurrentLocation());
	const bool can_favorite = favorite >= 0 || m_Source != 0 || EditorProject::Active();
	ImGui::SameLine(0.0f, 10.0f);
	ImGui::BeginDisabled(!can_favorite);
	if (ImGui::SmallButton(favorite >= 0 ? "Unfavorite" : "Favorite"))
	{
		if (favorite >= 0)
			s_Favorites.erase(s_Favorites.begin() + favorite);
		else if (!AddFavorite(CurrentLocation(), err))
			ELog.Msg(mtError, "Cannot add favorite: %s", err.c_str());
	}
	ImGui::EndDisabled();
	if (!can_favorite && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Open a project first");

	ImGui::SameLine();
	if (ImGui::SmallButton("Favorites...")) ImGui::OpenPopup("cb_favorites");
	DrawFavoritesPopup();

	ImGui::SameLine(0.0f, 14.0f);
	DrawBreadcrumbs();
	ImGui::PopID();
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
	EnsureListing();

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
					xr_string err;
					ApplyLocation(SLocation(1, i, ""), true, err);
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
		ImGui::TextColored(ThemeColor(XFinedTheme::ColorToken::Warning), "READ ONLY");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("nothing here is ever modified: browse, preview, and Copy out of it");
	}
	if (!m_Selection.empty())
	{
		ImGui::SameLine(0, 16);
		ImGui::TextDisabled("%d selected", int(m_Selection.size()));
	}
	if (!m_Clipboard.empty())
	{
		ImGui::SameLine(0, 12);
		ImGui::TextDisabled("| %d on the clipboard%s", int(m_Clipboard.size()), m_ClipCut ? " (cut)" : "");
	}

	ImGui::Separator();
	DrawNavigationBar();
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

	// both panes have had their frame at it
	m_ScrollToSelection = false;

	// Panel-wide, so the tree answers Del and Ctrl+C exactly like the grid.
	// Runs after both panes: Ctrl+A needs the drawn order the grid just built.
	HandleShortcuts();

	// destructive work is always one confirmation away
	if (m_ConfirmDelete && !ImGui::IsPopupOpen("Delete"))
		ImGui::OpenPopup("Delete");
	DrawConfirmDelete();

	ImGui::PopStyleVar(1);
	ImGui::End();
}
