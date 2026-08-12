#include "stdafx.h"
#include "EditorProject.h"
#include "EditorModManifest.h"
#include "EditorGameContent.h"
#include "XFinedMCP.h"
#include "UI_ToolsCustom.h"
#include "ui_main.h"
#include "UI_MainCommand.h"
#include "../XrEngine/XR_IOConsole.h"
#if defined(USE_DX11)
#include "EDX11Utils.h"
// the png encoder itself is compiled into RedImageTool, which XrECore links -
// this only pulls the declarations
#include "StbImage\stb_image_write.h"
#endif
#include <shobjidl.h>
#include <shellapi.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

//------------------------------------------------------------------------------
// state
//------------------------------------------------------------------------------
static const int kMaxRecent = 12;

struct SRecentProject
{
	string_path				path;
	string_path				name;
#if defined(USE_DX11)
	// the tile is drawn by ImGui, and ImGui binds a view, not a texture
	ID3D11ShaderResourceView*	preview;
#else
	IDirect3DTexture9*		preview;
#endif
	bool					preview_tried;
};

static char				s_AppRoot[MAX_PATH]	= {};
static char				s_Project[MAX_PATH]	= {};
static char				s_Name[MAX_PATH]	= {};
static char				s_BaseMaps[MAX_PATH]= {};
static bool				s_Active			= false;
static bool				s_WantImport		= false;
static SRecentProject	s_Recent[kMaxRecent];
static int				s_RecentCount		= 0;
// create-form fields
static char				s_NewParent[MAX_PATH]= {};
static char				s_NewName[64]		= {};
static char				s_PendingScene[MAX_PATH] = {};
// tile previews outlive Open() by one frame - the click happens mid-draw
static bool				s_ReleasePreviewsPending = false;
// ---- linked game install ----------------------------------------------------
static char				s_GameRoot[MAX_PATH]	= {};	// [project] game_root
static char				s_LinkError[256]		= {};	// last LinkGame() failure
// GameLinked() is polled from the draw loop and from every keystroke, so the
// three GetFileAttributes calls behind it are cached for a second. The timer
// also makes a game folder that disappears mid-session surface on its own.
static bool				s_LinkOk				= false;
static u32				s_LinkTick				= 0;
static bool				s_LinkFresh				= false;
// link-dialog fields, plus a cache so the live hint does not stat every frame
static char				s_LinkPath[MAX_PATH]	= {};
static char				s_LinkChecked[MAX_PATH]	= {};
static char				s_LinkHint[256]			= {};
static bool				s_LinkHintOk			= false;

// aliases whose writes must land inside the project; second column is the
// project-relative directory they get remounted to
static const char* kMount[][2] =
{
	{ "$maps$",				"rawdata\\levels"	},
	{ "$server_backup$",	"rawdata\\backup"	},
	{ "$game_levels$",		"gamedata\\levels"	},
	{ "$game_spawn$",		"gamedata\\spawns"	},
	{ "$import$",			"import"			},
	{ "$detail_objects$",	"import"			},
	{ "$omotion$",			"import"			},
	{ "$omotions$",			"import"			},
	{ "$smotion$",			"import"			},
	{ "$sbones$",			"import"			},
	{ "$temp$",				"temp"				},
	{ "$logs$",				"logs"				},
	{ "$game_saves$",		"savedgames"		},
	{ "$screenshots$",		"screenshots"		},
};
static const int kMountCount = sizeof(kMount)/sizeof(kMount[0]);
// original alias roots captured before the first mount, for clean unmount
static string_path		s_SavedRoot[kMountCount];
static bool				s_SavedRoots = false;

//------------------------------------------------------------------------------
// helpers
//------------------------------------------------------------------------------
static void ResolveAppRoot()
{
	if (s_AppRoot[0]) return;
	::GetModuleFileNameA(NULL, s_AppRoot, sizeof(s_AppRoot));
	for (int i = 0; i < 4; ++i)
		if (char* p = strrchr(s_AppRoot, '\\')) *p = 0;
	strcat_s(s_AppRoot, "\\");
	sprintf_s(s_BaseMaps, "%srawdata\\levels\\", s_AppRoot);
}

static void RecentIni(char* dst, u32 size)
{
	ResolveAppRoot();
	sprintf_s(dst, size, "%s_projects.ini", s_AppRoot);
}

static bool HasManifest(const char* folder)
{
	char p[MAX_PATH];
	sprintf_s(p, "%s\\project.ltx", folder);
	return ::GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static bool DirExists(const char* p)
{
	DWORD a = ::GetFileAttributesA(p);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EditorProject::PickFolder(char* dest, u32 dest_size, const wchar_t* title)
{
	bool ok = false;
	const HRESULT co = ::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	IFileDialog* dlg = 0;
	if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
	{
		DWORD opts = 0;
		dlg->GetOptions(&opts);
		dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
		dlg->SetTitle(title);
		if (SUCCEEDED(dlg->Show(NULL)))
		{
			IShellItem* item = 0;
			if (SUCCEEDED(dlg->GetResult(&item)))
			{
				PWSTR w = 0;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)))
				{
					::WideCharToMultiByte(CP_ACP, 0, w, -1, dest, dest_size, 0, 0);
					::CoTaskMemFree(w);
					ok = dest[0] != 0;
				}
				item->Release();
			}
		}
		dlg->Release();
	}
	if (co == S_OK) ::CoUninitialize();
	return ok;
}

bool EditorProject::ValidateName(LPCSTR name)
{
	if (!name || !name[0]) return false;
	for (const char* p = name; *p; ++p)
	{
		const char c = *p;
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
						(c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok) return false;
	}
	return true;
}

//------------------------------------------------------------------------------
// recents / manifest
//------------------------------------------------------------------------------
void EditorProject::LoadRecent()
{
	char ini[MAX_PATH];
	RecentIni(ini, sizeof(ini));
	s_RecentCount = 0;
	for (int i = 0; i < kMaxRecent; ++i)
	{
		char key[16];
		sprintf_s(key, "p%d", i);
		string_path v = {};
		::GetPrivateProfileStringA("recent", key, "", v, sizeof(v), ini);
		if (v[0] && DirExists(v) && HasManifest(v))
		{
			SRecentProject& r = s_Recent[s_RecentCount++];
			xr_strcpy(r.path, v);
			const char* leaf = strrchr(v, '\\');
			xr_strcpy(r.name, leaf ? leaf + 1 : v);
			r.preview = 0;
			r.preview_tried = false;
		}
	}
	::GetPrivateProfileStringA("recent", "create_parent", "", s_NewParent, sizeof(s_NewParent), ini);
	if (!s_NewParent[0])
		::ExpandEnvironmentStringsA("%USERPROFILE%\\Documents\\XFinedProjects", s_NewParent, sizeof(s_NewParent));
}

void EditorProject::SaveRecent()
{
	char ini[MAX_PATH];
	RecentIni(ini, sizeof(ini));
	// current project floats to the top, duplicates collapse
	string_path list[kMaxRecent];
	int n = 0;
	if (s_Project[0]) xr_strcpy(list[n++], s_Project);
	for (int i = 0; i < s_RecentCount && n < kMaxRecent; ++i)
		if (0 != _stricmp(s_Recent[i].path, s_Project))
			xr_strcpy(list[n++], s_Recent[i].path);
	for (int i = 0; i < kMaxRecent; ++i)
	{
		char key[16];
		sprintf_s(key, "p%d", i);
		::WritePrivateProfileStringA("recent", key, (i < n) ? list[i] : "", ini);
	}
	::WritePrivateProfileStringA("recent", "create_parent", s_NewParent, ini);
}

void EditorProject::LoadManifest()
{
	char p[MAX_PATH];
	sprintf_s(p, "%s\\project.ltx", s_Project);
	::GetPrivateProfileStringA("project", "game_root", "", s_GameRoot, sizeof(s_GameRoot), p);
	// trailing separators would double up in every <root>\... concatenation
	while (s_GameRoot[0] && (s_GameRoot[xr_strlen(s_GameRoot)-1] == '\\' ||
							 s_GameRoot[xr_strlen(s_GameRoot)-1] == '/'))
		s_GameRoot[xr_strlen(s_GameRoot)-1] = 0;

	s_LinkFresh		= false;		// force a real check on the next GameLinked()
	s_LinkError[0]	= 0;
	strcpy_s(s_LinkPath, s_GameRoot);	// seed the dialog with the stored value
	s_LinkChecked[0]= 0;
}

void EditorProject::SaveManifest()
{
	char p[MAX_PATH];
	sprintf_s(p, "%s\\project.ltx", s_Project);
	::WritePrivateProfileStringA("project", "name", s_Name, p);
	::WritePrivateProfileStringA("project", "sdk_root", s_AppRoot, p);
	::WritePrivateProfileStringA("project", "game_root", s_GameRoot, p);
	if (Tools && Tools->m_LastFileName.size())
		::WritePrivateProfileStringA("project", "last_scene", Tools->m_LastFileName.c_str(), p);
}

//------------------------------------------------------------------------------
// linked game install
//------------------------------------------------------------------------------
LPCSTR	EditorProject::GameRoot()	{ return s_GameRoot; }
LPCSTR	EditorProject::LinkError()	{ return s_LinkError; }

bool EditorProject::GameLinked()
{
	if (!s_GameRoot[0]) return false;

	const u32 now = ::GetTickCount();
	if (s_LinkFresh && (now - s_LinkTick) < 1000) return s_LinkOk;

	xr_string reason;
	s_LinkOk	= EditorGameContent::LooksLikeGame(s_GameRoot, reason);
	s_LinkTick	= now;
	s_LinkFresh	= true;
	return s_LinkOk;
}

bool EditorProject::LinkGame(LPCSTR folder)
{
	s_LinkError[0] = 0;
	if (!s_Active)
	{
		strcpy_s(s_LinkError, "no project is open");
		return false;
	}

	char clean[MAX_PATH] = {};
	strncpy_s(clean, sizeof(clean), folder ? folder : "", _TRUNCATE);
	for (char* p = clean; *p; ++p) if (*p == '/') *p = '\\';
	while (clean[0] && clean[xr_strlen(clean)-1] == '\\')
		clean[xr_strlen(clean)-1] = 0;

	xr_string reason;
	if (!EditorGameContent::LooksLikeGame(clean, reason))
	{
		strncpy_s(s_LinkError, sizeof(s_LinkError), reason.c_str(), _TRUNCATE);
		return false;
	}

	strcpy_s(s_GameRoot, clean);
	s_LinkFresh = false;
	SaveManifest();

	xr_string err;
	if (!EditorGameContent::Mount(s_GameRoot, err))
	{
		strncpy_s(s_LinkError, sizeof(s_LinkError), err.c_str(), _TRUNCATE);
		return false;
	}

	Msg("* Game linked: %s", s_GameRoot);
	return true;
}

void EditorProject::McpGameLink(LPCSTR raw, xr_string& out)
{
	char path[MAX_PATH] = {};
	const bool want_link = XFinedMCP::GetArg(raw, "path", path, sizeof(path)) && path[0];

	bool ok = true;
	xr_string reason;
	if (!s_Active)
	{
		ok		= false;
		reason	= "no project is open";
	}
	else if (want_link)
	{
		for (char* p = path; *p; ++p) if (*p == '/') *p = '\\';
		ok = LinkGame(path);
		if (!ok) reason = s_LinkError;
	}
	else if (s_GameRoot[0])
	{
		EditorGameContent::LooksLikeGame(s_GameRoot, reason);
	}
	else reason = "no game linked";

	// paths travel with forward slashes, same as the rest of the MCP layer
	xr_string root;
	for (const char* p = s_GameRoot; *p; ++p) root += (*p == '\\') ? '/' : *p;

	out  = ok ? "{\"ok\":true,\"linked\":" : "{\"ok\":false,\"linked\":";
	out += (s_Active && GameLinked()) ? "true" : "false";
	out += ",\"game_root\":\"";
	out += root;
	out += "\",\"reason\":\"";
	out += reason;
	out += "\"}";
}

//------------------------------------------------------------------------------
// mount / unmount
//------------------------------------------------------------------------------
void EditorProject::CreateLayout()
{
	static const char* dirs[] =
	{
		"Content",
		"rawdata", "rawdata\\levels", "rawdata\\backup",
		"gamedata", "gamedata\\levels", "gamedata\\spawns",
		"import", "temp", "logs", "savedgames", "screenshots",
	};
	for (int i = 0; i < (int)(sizeof(dirs)/sizeof(dirs[0])); ++i)
	{
		char p[MAX_PATH];
		sprintf_s(p, "%s\\%s", s_Project, dirs[i]);
		::CreateDirectoryA(p, NULL);
	}
}

void EditorProject::Mount()
{
	for (int i = 0; i < kMountCount; ++i)
	{
		FS_Path* P = FS.get_path(kMount[i][0]);
		if (!P) continue;
		if (!s_SavedRoots) xr_strcpy(s_SavedRoot[i], P->m_Path);

		string_path root;
		sprintf_s(root, "%s\\%s", s_Project, kMount[i][1]);
		P->_set_root(root);
		P->_set("");	// path = root exactly, whatever the original add part was
	}
	s_SavedRoots = true;
}

void EditorProject::Unmount()
{
	if (!s_SavedRoots) return;
	for (int i = 0; i < kMountCount; ++i)
	{
		FS_Path* P = FS.get_path(kMount[i][0]);
		if (!P) continue;
		P->_set_root(s_SavedRoot[i]);
		P->_set("");
	}
}

//------------------------------------------------------------------------------
// open / create / close
//------------------------------------------------------------------------------
bool EditorProject::Open(LPCSTR folder)
{
	if (!folder || !DirExists(folder)) return false;
	ResolveAppRoot();

	xr_strcpy(s_Project, folder);
	const char* leaf = strrchr(s_Project, '\\');
	xr_strcpy(s_Name, leaf ? leaf + 1 : s_Project);

	CreateLayout();		// heals missing subdirs in old/hand-made projects
	// XMS: silently add mod.ltx + module dirs to projects that predate them
	EditorMod::EnsureScaffold(s_Project, s_Name);
	LoadManifest();
	Mount();
	// typing in the browser may have woken the in-engine console — put it away
	if (Console) Console->Hide();
	SaveManifest();
	SaveRecent();
	// deferred: this can run inside the browser draw, see PopPendingScene()
	s_ReleasePreviewsPending = true;
	s_Active = true;

	// remember the last scene for a DEFERRED load — running COMMAND_LOAD here
	// would execute a heavy scene load inside the browser's popup stack
	char manifest[MAX_PATH];
	sprintf_s(manifest, "%s\\project.ltx", s_Project);
	::GetPrivateProfileStringA("project", "last_scene", "", s_PendingScene, sizeof(s_PendingScene), manifest);
	if (s_PendingScene[0] && ::GetFileAttributesA(s_PendingScene) == INVALID_FILE_ATTRIBUTES)
		s_PendingScene[0] = 0;	// stale path — skip silently

	Msg("* Project opened: %s", s_Project);
	return true;
}

void EditorProject::ListRecentJson(xr_string& out)
{
	char ini[MAX_PATH];
	RecentIni(ini, sizeof(ini));
	out = "[";
	bool first = true;
	for (int i = 0; i < kMaxRecent; ++i)
	{
		char key[16];
		sprintf_s(key, "p%d", i);
		string_path v = {};
		::GetPrivateProfileStringA("recent", key, "", v, sizeof(v), ini);
		if (!v[0] || !DirExists(v) || !HasManifest(v)) continue;
		const char* leaf = strrchr(v, '\\');
		char path[MAX_PATH];
		u32 o = 0;
		for (const char* p = v; *p && o + 2 < sizeof(path); ++p) path[o++] = (*p == '\\') ? '/' : *p;
		path[o] = 0;
		char tmp[MAX_PATH * 2];
		sprintf_s(tmp, "%s{\"name\":\"%s\",\"path\":\"%s\"}", first ? "" : ",", leaf ? leaf + 1 : v, path);
		out += tmp;
		first = false;
	}
	out += "]";
}

LPCSTR EditorProject::PopPendingScene()
{
	// Tile previews are freed here, one frame after Open(): clicking a tile
	// happens INSIDE the browser's draw, and that frame's ImGui draw list still
	// holds the texture pointer. Releasing it during Open() left the renderer
	// with a dangling ImTextureID and killed the editor on the spot.
	if (s_ReleasePreviewsPending)
	{
		ReleasePreviews();
		s_ReleasePreviewsPending = false;
	}

	if (!s_Active || !s_PendingScene[0]) return 0;
	static char out[MAX_PATH];
	strcpy_s(out, s_PendingScene);
	s_PendingScene[0] = 0;
	// crash insurance: clear last_scene BEFORE loading, so a load crash can't
	// lock the project out; a successful session rewrites it on close
	char manifest[MAX_PATH];
	sprintf_s(manifest, "%s\\project.ltx", s_Project);
	::WritePrivateProfileStringA("project", "last_scene", "", manifest);
	return out;
}

bool EditorProject::Create(LPCSTR parent, LPCSTR name)
{
	if (!ValidateName(name) || !parent || !parent[0]) return false;

	char folder[MAX_PATH];
	sprintf_s(folder, "%s\\%s", parent, name);
	if (DirExists(folder) && HasManifest(folder))
		return Open(folder);	// already a project — just open it

	// create the whole chain (parent may not exist yet)
	char acc[MAX_PATH] = {};
	for (const char* p = folder;; ++p)
	{
		if (*p == '\\' || *p == 0)
		{
			if (acc[0] && acc[xr_strlen(acc)-1] != ':') ::CreateDirectoryA(acc, NULL);
			if (*p == 0) break;
		}
		acc[p - folder] = *p;
		acc[p - folder + 1] = 0;
	}
	if (!DirExists(folder)) return false;
	EditorMod::EnsureScaffold(folder, name);	// XMS module skeleton
	return Open(folder);
}

void EditorProject::Close()
{
	if (!s_Active) return;
	SavePreview();
	SaveManifest();
	SaveRecent();
	Unmount();
	EditorGameContent::Unmount();	// closes the archive handles of the old link
	s_Active = false;
	s_Project[0] = 0;
	s_Name[0] = 0;
	s_GameRoot[0] = 0;
	s_LinkFresh = false;
	s_LinkError[0] = 0;
	LoadRecent();	// refresh tiles for the browser that is about to reappear
}

bool	EditorProject::Active()			{ return s_Active; }
LPCSTR	EditorProject::Root()			{ return s_Project; }
LPCSTR	EditorProject::Name()			{ return s_Name; }
LPCSTR	EditorProject::BaseMapsDir()	{ ResolveAppRoot(); return s_BaseMaps; }

void EditorProject::RequestImportScene()	{ s_WantImport = true; }

void EditorProject::OpenProjectFolder()
{
	ResolveAppRoot();
	::ShellExecuteA(NULL, "open", s_Active ? s_Project : s_AppRoot, NULL, NULL, SW_SHOWNORMAL);
}

//------------------------------------------------------------------------------
// previews
//------------------------------------------------------------------------------
void EditorProject::SavePreview()
{
	if (!s_Active || !HW.pDevice) return;
	// viewport render target only — the full backbuffer drags the tool panels
	// into the tile and looks like a screenshot of a spreadsheet
	if (!UI || !UI->RT->pSurface) return;
#if defined(USE_DX11)
	// staging copy instead of GetRenderTargetData; rows come back top-down, which
	// is the order the encoder wants
	U32Vec	px;
	u32		w = 0, h = 0;
	if (DX11ReadbackBGRA(UI->RT->pSurface, px, w, h))
	{
		// stb writes bytes in memory order: swap R and B, force the tile opaque
		for (u32& c : px)
			c = 0xff000000 | (c & 0x0000ff00) | ((c & 0x00ff0000) >> 16) | ((c & 0x000000ff) << 16);

		char p[MAX_PATH];
		sprintf_s(p, "%s\\preview.png", s_Project);
		stbi_write_png(p, int(w), int(h), 4, px.data(), int(w * sizeof(u32)));
	}
#else
	IDirect3DSurface9* surf = 0;
	if (SUCCEEDED(((IDirect3DTexture9*)UI->RT->pSurface)->GetSurfaceLevel(0, &surf)) && surf)
	{
		char p[MAX_PATH];
		sprintf_s(p, "%s\\preview.png", s_Project);
		D3DXSaveSurfaceToFileA(p, D3DXIFF_PNG, surf, NULL, NULL);
		surf->Release();
	}
#endif
}

#if defined(USE_DX11)
static ID3D11ShaderResourceView* LoadPreview(const char* project)
{
	char p[MAX_PATH];
	sprintf_s(p, "%s\\preview.png", project);
	if (::GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return 0;
	return DX11TextureFromFile(p);
}
#else
static IDirect3DTexture9* LoadPreview(const char* project)
{
	char p[MAX_PATH];
	sprintf_s(p, "%s\\preview.png", project);
	if (::GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return 0;
	IDirect3DTexture9* tex = 0;
	if (FAILED(D3DXCreateTextureFromFileExA(HW.pDevice, p, D3DX_DEFAULT, D3DX_DEFAULT, 1, 0,
		D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, D3DX_FILTER_LINEAR, D3DX_FILTER_NONE, 0, NULL, NULL, &tex)))
		return 0;
	return tex;
}
#endif

void EditorProject::ReleasePreviews()
{
	for (int i = 0; i < s_RecentCount; ++i)
	{
		if (s_Recent[i].preview) s_Recent[i].preview->Release();
		s_Recent[i].preview = 0;
		s_Recent[i].preview_tried = false;
	}
}

//------------------------------------------------------------------------------
// the browser page (fullscreen, drawn instead of the editor UI)
//------------------------------------------------------------------------------
// -project <folder|name>: opens that project at startup and skips the picker
// entirely, so a scripted run - a smoke test, an agent driving the MCP port -
// lands in the right project without anyone clicking a tile. The argument is
// either a project folder or the name of a recent one; quotes are honoured
// because a path with spaces is the normal case.
static bool OpenProjectFromCommandLine()
{
	LPCSTR cmd = ::GetCommandLineA();
	LPCSTR at  = strstr(cmd, "-project");
	if (!at)	return false;

	LPCSTR p = at + xr_strlen("-project");
	while (' ' == *p || '=' == *p || ':' == *p) ++p;
	if (!*p)	return false;

	string_path arg = {};
	if ('"' == *p)
	{
		++p;
		LPCSTR end = strchr(p, '"');
		const int len = end ? int(end - p) : int(xr_strlen(p));
		strncpy_s(arg, sizeof(arg), p, _TRUNCATE);
		if (len < (int)sizeof(arg)) arg[len] = 0;
	}
	else
	{
		LPCSTR end = strchr(p, ' ');
		const int len = end ? int(end - p) : int(xr_strlen(p));
		strncpy_s(arg, sizeof(arg), p, _TRUNCATE);
		if (len < (int)sizeof(arg)) arg[len] = 0;
	}
	if (!arg[0])	return false;

	for (char* c = arg; *c; ++c) if ('/' == *c) *c = '\\';

	if (EditorProject::Open(arg))
	{
		Msg("* -project: opened '%s'", arg);
		return true;
	}

	// not a folder - try it as the NAME of a project we already know
	for (int i = 0; i < s_RecentCount; ++i)
		if (0 == _stricmp(s_Recent[i].name, arg) && EditorProject::Open(s_Recent[i].path))
		{
			Msg("* -project: opened recent '%s'", s_Recent[i].path);
			return true;
		}

	Msg("! -project: cannot open '%s' - falling back to the project browser", arg);
	return false;
}

bool EditorProject::DrawBrowser()
{
	if (s_Active) return false;

	static bool s_loaded = false;
	if (!s_loaded)
	{
		ResolveAppRoot(); LoadRecent(); s_loaded = true;
		// done once, after the recent list is up: matching by name needs it
		if (OpenProjectFromCommandLine())	return false;
	}

	// the editor's proven self-opening modal (imgui_user overload) — the same
	// path UIChooseForm uses, so it is guaranteed to be visible and focused
	ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
	if (!ImGui::BeginPopupModal("XFined Editor - Projects", nullptr, 0, true))
		return true;

	ImGui::TextDisabled("Select a project or create a new one");
	ImGui::Separator();

	// ---- recent tiles ----------------------------------------------------
	const float tile_w = 220.f, tile_h = 150.f;
	if (ImGui::BeginChild("tiles", ImVec2(0, -170), false))
	{
		if (!s_RecentCount)
			ImGui::TextDisabled("no recent projects — create one below");

		const float avail = ImGui::GetContentRegionAvail().x;
		int per_row = (int)(avail / (tile_w + 12.f));
		if (per_row < 1) per_row = 1;

		for (int i = 0; i < s_RecentCount; ++i)
		{
			SRecentProject& r = s_Recent[i];
			if (!r.preview_tried) { r.preview = LoadPreview(r.path); r.preview_tried = true; }

			if (i % per_row) ImGui::SameLine();
			ImGui::PushID(i);
			ImGui::BeginGroup();

			bool clicked;
			if (r.preview)
				clicked = ImGui::ImageButton((ImTextureID)r.preview, ImVec2(tile_w, tile_h - 30));
			else
				clicked = ImGui::Button(r.name, ImVec2(tile_w + 8, tile_h - 22));

			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tile_w);
			ImGui::TextUnformatted(r.name);
			ImGui::TextDisabled("%s", r.path);
			ImGui::PopTextWrapPos();
			ImGui::EndGroup();
			ImGui::PopID();

			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.path);
			// Open() mutates the recent list — leave the loop immediately
			if (clicked) { if (Open(r.path)) ImGui::CloseCurrentPopup(); break; }
		}
	}
	ImGui::EndChild();

	// ---- create / open ---------------------------------------------------
	ImGui::Separator();
	ImGui::TextUnformatted("New project");
	ImGui::SetNextItemWidth(420);
	ImGui::InputText("##parent", s_NewParent, sizeof(s_NewParent));
	ImGui::SameLine();
	if (ImGui::Button("Browse..."))
	{
		char picked[MAX_PATH] = {};
		if (PickFolder(picked, sizeof(picked), L"Select the folder that will contain the project"))
			strcpy_s(s_NewParent, picked);
	}
	ImGui::SameLine(); ImGui::TextDisabled("parent folder");

	ImGui::SetNextItemWidth(240);
	ImGui::InputText("##name", s_NewName, sizeof(s_NewName));
	ImGui::SameLine(); ImGui::TextDisabled("name: latin letters, digits, _ and -");

	const bool name_ok = ValidateName(s_NewName);
	if (!name_ok && s_NewName[0])
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "invalid name");

	ImGui::BeginDisabled(!name_ok || !s_NewParent[0]);
	if (ImGui::Button("Create", ImVec2(140, 0)))
	{
		if (Create(s_NewParent, s_NewName)) ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();

	ImGui::SameLine(0, 24);
	if (ImGui::Button("Open Existing...", ImVec2(140, 0)))
	{
		char picked[MAX_PATH] = {};
		if (PickFolder(picked, sizeof(picked), L"Select a project folder (must contain project.ltx)"))
		{
			if (HasManifest(picked))	{ if (Open(picked)) ImGui::CloseCurrentPopup(); }
			else						ELog.DlgMsg(mtError, "Not a project folder: no project.ltx inside.");
		}
	}
	ImGui::SameLine(0, 24);
	if (ImGui::Button("Exit", ImVec2(100, 0)))
		UI->Quit();

	ImGui::EndPopup();
	return true;
}

//------------------------------------------------------------------------------
// the Link Game page (blocking, drawn instead of the whole editor UI)
//
// Same mechanism as DrawBrowser: the self-opening imgui_user overload re-opens
// the popup on every frame it is still needed, so Escape (which pops the modal
// inside ImGui) cannot actually dismiss it - it is back before anything is
// drawn. p_open is nullptr, so there is no close button either. The only ways
// out are a valid link or Exit Editor.
//------------------------------------------------------------------------------
bool EditorProject::DrawGameLink()
{
	if (!s_Active)		return false;
	if (GameLinked())	return false;

	ImGui::SetNextWindowSize(ImVec2(760, 320), ImGuiCond_FirstUseEver);
	if (!ImGui::BeginPopupModal("XFined Editor - Link Game", nullptr, 0, true))
		return true;

	ImGui::TextUnformatted("This project is not linked to a game install yet.");
	ImGui::TextDisabled("Pick the folder that holds fsgame.ltx and database\\ (or gamedata\\).");
	ImGui::TextDisabled("The editor reads that folder only - it never writes anything into it.");
	ImGui::Separator();

	ImGui::SetNextItemWidth(560);
	ImGui::InputText("##gameroot", s_LinkPath, sizeof(s_LinkPath));
	ImGui::SameLine();
	if (ImGui::Button("Browse..."))
	{
		char picked[MAX_PATH] = {};
		if (PickFolder(picked, sizeof(picked), L"Select the game folder (must contain fsgame.ltx)"))
			strcpy_s(s_LinkPath, picked);
	}
	ImGui::SameLine(); ImGui::TextDisabled("game folder");

	// live hint, re-evaluated only when the text actually changed
	if (0 != strcmp(s_LinkPath, s_LinkChecked))
	{
		strcpy_s(s_LinkChecked, s_LinkPath);
		xr_string reason;
		s_LinkHintOk = EditorGameContent::LooksLikeGame(s_LinkPath, reason);
		if (s_LinkHintOk)	strcpy_s(s_LinkHint, "valid Dead Air install");
		else				strncpy_s(s_LinkHint, sizeof(s_LinkHint), reason.c_str(), _TRUNCATE);
	}
	if (s_LinkHintOk)
		ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "%s", s_LinkHint);
	else
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", s_LinkHint[0] ? s_LinkHint : "no folder selected");

	if (s_LinkError[0])
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "link failed: %s", s_LinkError);

	ImGui::Separator();
	ImGui::BeginDisabled(!s_LinkHintOk);
	if (ImGui::Button("Link", ImVec2(140, 0)))
	{
		if (LinkGame(s_LinkPath))
		{
			s_LinkChecked[0] = 0;
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::EndDisabled();

	ImGui::SameLine(0, 24);
	if (ImGui::Button("Exit Editor", ImVec2(140, 0)))
	{
		// same command as File\Quit; CommandQuit only forwards to UI->Quit()
		// when the scene is dirty, so back it up - nobody may get stuck here
		ExecCommand(COMMAND_QUIT);
		if (UI) UI->Quit();
	}
	ImGui::SameLine(0, 24);
	ImGui::TextDisabled("project: %s", s_Project);

	ImGui::EndPopup();
	return true;
}

//------------------------------------------------------------------------------
// Import Base Scene modal (unchanged behaviour)
//------------------------------------------------------------------------------
static bool CopyTree(const char* src, const char* dst)
{
	::CreateDirectoryA(dst, NULL);
	char mask[MAX_PATH];
	sprintf_s(mask, "%s\\*", src);
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (h == INVALID_HANDLE_VALUE) return false;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		char a[MAX_PATH], b[MAX_PATH];
		sprintf_s(a, "%s\\%s", src, fd.cFileName);
		sprintf_s(b, "%s\\%s", dst, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)	CopyTree(a, b);
		else												::CopyFileA(a, b, FALSE);
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
	return true;
}

void EditorProject::DrawUI()
{
	if (!s_WantImport || !s_Active) return;

	ImGui::OpenPopup("Import Base Scene");
	ImGui::SetNextWindowSize(ImVec2(420, 460), ImGuiCond_FirstUseEver);
	if (ImGui::BeginPopupModal("Import Base Scene", &s_WantImport))
	{
		ImGui::TextDisabled("SDK scenes: %s", s_BaseMaps);
		ImGui::Separator();
		if (ImGui::BeginChild("scenes", ImVec2(0, -32)))
		{
			char mask[MAX_PATH];
			sprintf_s(mask, "%s*.level", s_BaseMaps);
			WIN32_FIND_DATAA fd;
			HANDLE h = ::FindFirstFileA(mask, &fd);
			if (h != INVALID_HANDLE_VALUE)
			{
				do
				{
					if (ImGui::Selectable(fd.cFileName))
					{
						char name[MAX_PATH];
						strcpy_s(name, fd.cFileName);
						if (char* dot = strrchr(name, '.')) *dot = 0;

						string_path src_file, dst_file, src_dir, dst_dir;
						sprintf_s(src_file, "%s%s", s_BaseMaps, fd.cFileName);
						sprintf_s(dst_file, "%s\\rawdata\\levels\\%s", s_Project, fd.cFileName);
						sprintf_s(src_dir, "%s%s", s_BaseMaps, name);
						sprintf_s(dst_dir, "%s\\rawdata\\levels\\%s", s_Project, name);

						::CopyFileA(src_file, dst_file, FALSE);
						if (DirExists(src_dir)) CopyTree(src_dir, dst_dir);

						ELog.Msg(mtInformation, "Scene '%s' imported into the project.", name);
						s_WantImport = false;
					}
				} while (::FindNextFileA(h, &fd));
				::FindClose(h);
			}
			else ImGui::TextDisabled("no scenes found");
		}
		ImGui::EndChild();
		ImGui::Separator();
		if (ImGui::Button("Close", ImVec2(-1, 0))) s_WantImport = false;
		ImGui::EndPopup();
	}
}
