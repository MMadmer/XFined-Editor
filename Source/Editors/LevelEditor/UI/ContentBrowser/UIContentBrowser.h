#pragma once

// Unreal-style dockable asset browser.
//
// It does not enumerate anything itself: every asset kind is already registered
// in UIChooseForm's event table (see FillChooseEvents), which supplies both a
// fill function and a thumbnail adapter. This panel just renders that data as a
// folder tree plus a thumbnail grid, and can spawn what it shows into the scene.

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

	// asset currently held by an in-flight drag, empty when nothing is dragged
	static LPCSTR	DraggedAsset		();
	static u32		DraggedCategory		();

	// The model preview window registers itself here instead of being referenced
	// directly, so the browser does not depend on it existing. Without a handler
	// a double click on a model just reports that no viewer is installed.
	typedef void (*TShowVisual)		(LPCSTR visual_name);
	typedef void (*TShowVisualMem)	(LPCSTR title, const void* data, u32 size);
	static void		SetVisualPreview	(TShowVisual by_name, TShowVisualMem from_memory);

	// asset categories, shared with the MCP asset commands
	static int		CategoryCount		();
	static LPCSTR	CategoryName		(int index);		// caption, e.g. "Objects"
	static u32		CategoryId			(int index);		// EChooseMode id
	static int		FindCategory		(LPCSTR name);		// by caption, case-insensitive; -1 = unknown

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
	SFolder							m_Root;
	xr_string						m_CurFolder;
	u32								m_Category;
	// UE-style split: 0 = project "Content" folder (disk scan), 1 = shared
	// "Editor Content" (UIChooseForm event table), 2 = "DARF Content" (the
	// linked game install, STRICTLY READ ONLY - see IsReadOnlySource)
	int								m_Source;
	bool							m_NeedRefresh;
	float							m_TileSize;
	// folder-tree width, dragged by the splitter between the two panes and
	// persisted in the editor preferences
	float							m_TreeWidth;
	ImGuiTextFilter					m_Filter;
	// UE-style multi-selection: click replaces, Ctrl toggles, Shift takes the
	// range from the anchor. The range needs the order tiles were drawn in, so
	// the grid records it and a shift-click is resolved after the loop.
	xr_vector<xr_string>			m_Selection;
	xr_string						m_Anchor;
	xr_vector<xr_string>			m_DrawnOrder;
	xr_string						m_PendingRange;
	xr_string						m_Dragged;

	// Copy/paste clipboard. Holds names plus the source they came from, because
	// where an item lives decides how it is fetched on paste.
	xr_vector<xr_string>			m_Clipboard;
	int								m_ClipSource;

	// DARF source state
	bool							m_DarfReady;	// mounted and browsable
	xr_string						m_DarfStatus;	// why not, when it is not
	bool							m_WantOverwrite;// overwrite-confirm popup
	xr_string						m_CopyPending;	// item awaiting confirmation
	xr_string						m_CopyTarget;
	// the game tree dwarfs the SDK one: cap how much work one frame may do
	u32								m_ThumbsThisFrame;

	void			Refresh				();
	void			BuildTree			();
	void			DrawFolder			(SFolder& f);
	void			DrawTiles			();
	void			CollectItems		(SFolder& f, xr_vector<int>& out, bool recursive);
	SFolder*		FindFolder			(LPCSTR path);
	ImTextureID		GetThumb			(LPCSTR name);
	// queues an offscreen model render; the result is picked up on a later frame
	void			RequestModelThumbnail(LPCSTR name);
	// double-click handler: opens the asset's viewer, never touches the scene
	void			OpenAsset			(LPCSTR name);
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

	// selection
	bool			IsSelected			(LPCSTR full) const;
	void			SelectItem			(LPCSTR full, bool additive, bool range);
	void			ClearSelection		();
	void			ApplyPendingRange	();

	void			RequestCopy			(LPCSTR rel);
	void			CopySelection		();
	void			DrawCopyConfirm		();

	// clipboard
	void			ClipboardCopy		();
	void			ClipboardPaste		();
	// true when the current source/folder can receive a paste
	bool			CanPaste			() const;
	LPCSTR			PasteBlockedReason	() const;
	// Ctrl+C / Ctrl+V while the grid has focus
	void			HandleShortcuts		();
	// right-click on empty grid space
	void			DrawGridContextMenu	();
	// right-click surface for a tile; contents depend on the source
	void			DrawItemContextMenu	(LPCSTR full);
	// right-click surface for a folder tile / tree node
	void			DrawFolderContextMenu(LPCSTR path);
};
