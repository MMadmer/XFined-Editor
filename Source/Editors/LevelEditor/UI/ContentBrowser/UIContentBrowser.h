#pragma once

// Unreal-style dockable asset browser.
//
// It does not enumerate anything itself: every asset kind is already registered
// in UIChooseForm's event table (see FillChooseEvents), which supplies both a
// fill function and a thumbnail adapter. This panel just renders that data as a
// folder tree plus a thumbnail grid, and can spawn what it shows into the scene.

// every write goes through the file-op layer, whose report type is part of the
// paste signature below
#include "../../../XrECore/Editor/EditorFileOps.h"

// payload id for viewport drag&drop
#define CB_DND_PAYLOAD "XRAY_ASSET"

class UIContentBrowser : public XrUI
{
public:
					UIContentBrowser	();
	virtual			~UIContentBrowser	();
	virtual void	Draw				();

	static void		Update				();
	static void		Show				();
	static void		Close				();
	static IC bool	IsOpen				() { return !!Form; }

	// The viewport drop hands over the browser's own name for the asset; what
	// placement needs is a library reference. Translates per the ACTIVE source:
	// an Objects entry is already a ref, a game path resolves through
	// ResolvePlaceable, a project .object strips its rawdata prefix. False =
	// not placeable (textures, sounds, meshes without an .object, ...).
	static bool		ResolveDropRef		(LPCSTR name, string_path& ref);

	// asset currently held by an in-flight drag, empty when nothing is dragged
	static LPCSTR	DraggedAsset		();
	static u32		DraggedCategory		();

	// The model preview window registers itself here instead of being referenced
	// directly, so the browser does not depend on it existing. Without a handler
	// a double click on a model just reports that no viewer is installed.
	typedef void (*TShowVisual)		(LPCSTR visual_name);
	typedef void (*TShowVisualMem)	(LPCSTR title, const void* data, u32 size);
	// `object_from_memory` takes the editor's .object format rather than an
	// engine .ogf - the project source hands over raw file bytes for both
	static void		SetVisualPreview	(TShowVisual by_name, TShowVisualMem from_memory,
										 TShowVisualMem object_from_memory);

	// MCP: reveal an asset in the browser - switches source and category when
	// needed, navigates to the folder holding it, selects it and optionally
	// opens its viewer (the double-click action). Returns false + a reason when
	// the asset is not in the current listing.
	//   source: -1 keeps the current one, else 0 project / 1 editor / 2 game
	static bool		RevealAsset			(LPCSTR name, int source, bool open_viewer, xr_string& err);
	// MCP: what is selected right now, and in which source/folder
	static void		GetSelection		(int& source, xr_string& folder, xr_vector<xr_string>& sel);
	// MCP: copy items - or a whole folder, subfolders included - out of a
	// read-only source into the project. Unlike the menu this never prompts;
	// `overwrite` decides. `names` is a ';'-separated list and may be empty when
	// `folder` is given. source/category of -1 keep whatever the browser is on.
	// `dst` empty mirrors each asset into the layout fs.ltx implies; give it a
	// project-relative folder and it goes through the browser's own clipboard
	// and paste instead, i.e. exactly where you asked for it.
	static bool		McpCopyToProject	(LPCSTR names, LPCSTR folder, LPCSTR dst,
										 int source, int category,
										 bool overwrite, int& files, xr_string& err);

	// asset categories, shared with the MCP asset commands
	static int		CategoryCount		();
	static LPCSTR	CategoryName		(int index);		// caption, e.g. "Objects"
	static u32		CategoryId			(int index);		// EChooseMode id
	static int		FindCategory		(LPCSTR name);		// by caption, case-insensitive; -1 = unknown
	// Levels have no EChooseMode - they are scenes, not library assets - so the
	// callers that enumerate categories have to special-case them
	static bool		IsLevelsCategory	(int index);

private:
	struct SFolder
	{
		xr_string					name;		// leaf name
		xr_string					path;		// full path, '\\'-separated
		xr_vector<SFolder>			children;
		xr_vector<int>				items;		// indices into m_Items
	};

	static UIContentBrowser*		Form;

	// thumbnail cache — the choose-event adapters allocate a fresh D3D texture
	// per call, far too costly for a grid, so results are kept keyed by name
	struct SThumb
	{
		ImTextureID					tex;
		u32							last_used;
	};
	DEFINE_MAP(shared_str, SThumb, ThumbMap, ThumbMapIt);
	ThumbMap						m_Thumbs;
	u32								m_Tick;

	ChooseItemVec					m_Items;
	// Project source only: the folders that exist on disk. The tree is otherwise
	// derived from item names, which cannot describe an EMPTY folder - and a
	// freshly created one is exactly that.
	xr_vector<xr_string>			m_Dirs;
	SFolder							m_Root;
	xr_string						m_CurFolder;
	// what the grid drew last frame, so entering a folder can start at its top
	xr_string						m_ShownFolder;
	u32								m_Category;
	// UE-style split: 0 = project "Content" folder (disk scan), 1 = shared
	// "Editor Content" (UIChooseForm event table), 2 = "DARF Content" (the
	// linked game install, STRICTLY READ ONLY - see IsReadOnlySource)
	int								m_Source;
	bool							m_NeedRefresh;
	float							m_TileSize;
	// What one tile really occupies: m_TileSize is the PICTURE, a button adds
	// its frame padding around it. Every tile is drawn at this width and the
	// row wrap is computed from it, so folders and assets line up in the same
	// columns and the last column stops falling off the right edge.
	float							TileWidth() const;
	// folder-tree width, dragged by the splitter between the two panes and
	// persisted in the editor preferences
	float							m_TreeWidth;
	ImGuiTextFilter					m_Filter;
	// One entry of the grid. A folder and an asset are THE SAME THING to the
	// selection and to copy/cut/paste/delete - only the code that finally has to
	// touch the disk looks at `folder`. Nothing else is allowed to branch on it,
	// or a mixed selection stops behaving like one selection.
	struct SEntry
	{
		xr_string					path;
		bool						folder;
									SEntry() : folder(false) {}
									SEntry(LPCSTR p, bool f) : path(p), folder(f) {}
		bool operator==				(const SEntry& o) const { return folder == o.folder && path == o.path; }
	};

	// UE-style multi-selection: click replaces, Ctrl toggles, Shift takes the
	// range from the anchor. The range needs the order tiles were drawn in, so
	// the grid records it and a shift-click is resolved after the loop.
	xr_vector<SEntry>				m_Selection;
	SEntry							m_Anchor;
	bool							m_HasAnchor;
	xr_vector<SEntry>				m_DrawnOrder;
	// screen rect of each drawn tile, parallel to m_DrawnOrder - the rubber-band
	// needs to know what it swept over
	xr_vector<ImVec4>				m_DrawnRects;
	bool							m_Marquee;
	ImVec2							m_MarqueeStart;
	xr_vector<SEntry>				m_MarqueeBase;	// selection when the drag began
	SEntry							m_PendingRange;
	bool							m_HasPendingRange;
	xr_string						m_Dragged;

	// A selection made from OUTSIDE the panel (a reveal over MCP) has to end up
	// on screen, the way the outliner scrolls to a viewport pick: the tree opens
	// the chain down to the current folder, the grid scrolls to the LAST
	// selected tile in grid order. Nobody scrolls a thousand tiles by hand. The
	// panel's own clicks never raise it - their tile is already visible.
	// Cleared once both panes have had their frame, so a reveal into a collapsed
	// window still lands when the window comes back.
	bool							m_ScrollToSelection;

	// Clipboard. Copy/Cut put things HERE; nothing is written until a paste says
	// where. Folders coming out of a read-only source are expanded at copy time,
	// because the listing that turns a folder into its items is the SOURCE's -
	// and by paste time the browser is looking at the project instead.
	struct SClip
	{
		xr_string					src;	// how the source names it
		xr_string					rel;	// where it goes under the paste target
		bool						folder;	// only ever true for project entries
	};
	xr_vector<SClip>				m_Clipboard;
	int								m_ClipSource;
	// ...and the category it was copied from: an Editor Content name is only
	// resolvable against the aliases of ITS category, and the user is free to
	// switch categories between the copy and the paste
	u32								m_ClipCategory;
	bool							m_ClipCut;		// Cut, so the paste moves

	// DARF source state
	bool							m_DarfReady;	// mounted and browsable
	xr_string						m_DarfStatus;	// why not, when it is not
	// the game tree dwarfs the SDK one: cap how much work one frame may do
	u32								m_ThumbsThisFrame;

	void			Refresh				();
	// Refresh(), plus the project check that has to precede it: the panel can
	// be drawn (and listed empty) before -project has opened anything, and
	// nothing invalidated that listing afterwards.
	void			EnsureListing		();
	// project the listing was built for; "" when none was open
	xr_string						m_ListedProject;
	void			BuildTree			();
	// creates the folder chain for `path` and returns its node
	SFolder*		EnsureFolder		(LPCSTR path);
	void			DrawFolder			(SFolder& f);
	void			DrawTiles			();
	void			CollectItems		(SFolder& f, xr_vector<int>& out, bool recursive);
	SFolder*		FindFolder			(LPCSTR path);
	ImTextureID		GetThumb			(LPCSTR name);
	// queues an offscreen model render; the result is picked up on a later frame
	void			RequestModelThumbnail(LPCSTR name);
	// Double-click handler: opens the asset's viewer, never touches the scene.
	// `err` non-null returns the reason instead of popping a dialog, which is
	// what MCP needs - a modal on an unattended machine is a hang.
	bool			OpenAsset			(LPCSTR name, xr_string* err = 0);
	// a listing entry -> the .level file COMMAND_LOAD can open. `source` is the
	// browser's: the project names files relative to its root, the library by
	// a name under $maps$ with the extension clamped off.
	static bool		ResolveLevelFile	(LPCSTR name, int source, string_path& out);
	void			DropCache			();
	void			SwitchSource		(int src);
	// vertical splitter between the folder tree and the tile grid
	void			DrawSplitter		(float height);
	// The project's own Content is the only writable root. Editor Content is a
	// shared SDK library and the game install is someone else's install - both
	// are browse-and-copy-out sources, never edited in place.
	IC bool			IsReadOnlySource	() const { return m_Source != 0; }
	// the linked game install specifically: it owns the copy machinery and the
	// per-frame tile budget its size demands
	IC bool			IsGameSource		() const { return m_Source == 2; }

	// selection - folders and assets go through exactly the same calls
	bool			IsSelected			(LPCSTR path, bool folder) const;
	void			SelectEntry			(LPCSTR path, bool folder, bool additive, bool range);
	void			ClearSelection		();
	void			ApplyPendingRange	();
	// rubber-band selection over the tile grid
	void			UpdateMarquee		();

	// Editor Content -> disk. The item is a library reference, so the files
	// behind it are resolved through the fs.ltx aliases first; see LibFilesFor.
	// Returns how many files actually landed.
	int				CopyLibraryItem		(u32 category, LPCSTR ref, LPCSTR dst_dir,
										 bool overwrite, xr_string& err);
	// the copy MCP runs: mirrors each asset into the layout fs.ltx implies
	int				CopyRefsToProject	(u32 category, const xr_vector<xr_string>& names,
										 bool overwrite, xr_string& err);

	// folder authoring, project source only
	void			CreateFolder		();				// in the folder now open
	void			BeginRename			(LPCSTR path);	// inline edit on that tile
	void			CommitRename		();
	// Inline rename of a folder tile: the created folder starts here with its
	// name fully selected, so typing replaces it - the usual file-manager flow.
	xr_string						m_Rename;		// folder path being renamed, "" = none
	char							m_RenameBuf[128];
	bool							m_RenameFocus;	// grab the keyboard on the next frame

	// clipboard - one set of commands for whatever is selected
	void			ClipboardCopy		(bool cut);
	void			ClipboardPaste		();
	// the paste itself, into an explicit absolute folder: shared with MCP so a
	// scripted paste goes through exactly the code the menu does
	int				PasteClipboardInto	(LPCSTR dst_root, bool overwrite, xr_string& err);
	// Deleting content has no undo of its own, so it always asks first: this
	// only arms the confirmation, DeleteConfirmed does the work. Everything
	// destructive in this panel goes through that pair.
	void			DeleteSelection		();
	void			DeleteConfirmed		();
	void			DrawConfirmDelete	();
	bool							m_ConfirmDelete;	// modal is armed
	xr_string						m_ConfirmText;		// what is about to go
	// materialises one clipboard entry under `dst_dir`
	void			PasteEntry			(const SClip& c, LPCSTR dst_dir, bool overwrite,
										 EditorFileOps::SReport& rep);
	// expands a folder of a read-only source into the clipboard entries of the
	// items under it, each keeping its place below the folder's own name
	void			ClipExpandFolder	(LPCSTR path);
	// true when the current source/folder can receive a paste
	bool			CanPaste			() const;
	LPCSTR			PasteBlockedReason	() const;
	// Ctrl+C / Ctrl+V while the grid has focus
	void			HandleShortcuts		();
	// right-click on empty grid space
	void			DrawGridContextMenu	();
	// Right-click surface for ANY tile. One menu for folders and assets alike -
	// they are the same thing to every command on it.
	void			DrawEntryContextMenu(LPCSTR path, bool folder);
};
