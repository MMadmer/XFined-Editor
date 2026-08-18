#pragma once

// NQ - the graph canvas of a quest document (docs/NQ_ARCHITECTURE.md par. 13.8).
// Own ImDrawList implementation: infinite world, right-button pan, ten fixed zoom
// levels, top-down flow (input on the top edge, one output pin per catalog pin on
// the bottom edge), nodes with an on_enter chip strip above the body and an
// on_exit strip below it, triggers as ovals. Every edit goes through NqDoc so
// undo/redo and MCP see the same operations.

#include "../../../XrECore/Editor/Nq/NqDoc.h"
#include "../../../XrECore/Editor/Nq/NqCatalog.h"

class NqCanvas
{
public:
	struct SViewState
	{
		float				cx, cy;
		int					zoom;
		xr_vector<xr_string>	selection;
		xr_string				slot;
	};

	explicit		NqCanvas		(NqDoc* doc);

	// draws the canvas into the current window at the cursor, `size` pixels
	void			Draw			(const ImVec2& size);

	// view control (also used by MCP quest_view)
	static int		ZoomLevels		();
	static float	ZoomOf			(int idx);
	void			SetZoom			(int idx);				// clamps
	void			ZoomBy			(int delta, const ImVec2* anchor_screen = 0);
	void			FrameAll		();
	void			FrameSelection	();
	void			FrameNode		(LPCSTR id);
	// an explicit centre wins over a pending frame request (the coordinates are
	// clamped: quest_view takes them straight from the request line)
	void			Center			(float wx, float wy);
	// all/selection framing needs the canvas rect and waits for Draw when needed;
	// a single node can be centred immediately
	void			RequestFrameAll	();
	void			RequestFrameSel	();
	void			RequestFrameNode(LPCSTR id);
	bool			FramePending	() const { return m_WantFrameAll || m_WantFrameSel; }
	void			BeginNavigationBatch();
	void			EndNavigationBatch();
	bool			HistoryBack		();
	bool			HistoryForward	();
	void			ClearHistory	();
	SViewState		CurrentView		() const;
	const xr_vector<SViewState>& BackHistory() const	{ return m_BackHistory; }
	const xr_vector<SViewState>& ForwardHistory() const { return m_ForwardHistory; }

	// Bookmarks are transient editor state and never enter the quest asset.
	bool			SetBookmark		(LPCSTR id, bool enabled);
	bool			ToggleBookmark	(LPCSTR id);
	void			ClearBookmarks	();
	bool			JumpBookmark	(int delta);
	bool			IsBookmarked	(LPCSTR id) const;
	const xr_vector<xr_string>& Bookmarks() const		{ return m_Bookmarks; }

	void			SetMinimapVisible(bool visible)
	{
		m_ShowMinimap = visible;
		if (!visible) m_MinimapHovered = m_MinimapPanning = false;
	}
	bool			MinimapVisible	() const					{ return m_ShowMinimap; }
	bool			GraphBounds		(float& min_x, float& min_y, float& max_x, float& max_y) const;
	bool			ViewportBounds	(float& min_x, float& min_y, float& max_x, float& max_y) const;

	// the inspector wants the id field focused (F2 / rename)
	bool			TakeRenameRequest()						{ bool r = m_WantRename; m_WantRename = false; return r; }
	// the inspector wants the action section focused (chip double click)
	bool			TakeFocusAction	()						{ bool r = m_WantFocusAction; m_WantFocusAction = false; return r; }

	// clipboard: selected nodes as a Lua fragment
	void			CopySelection	();
	void			PasteClipboard	();
	void			DeleteSelection	();
	void			SelectAll		();
	// Replaces the selection with exactly these node ids (empty = clear). The one
	// way in from outside the canvas; the piecewise helpers below are its internals.
	void			SelectNodes		(const xr_vector<xr_string>& ids);
	void			DuplicateSelection();
	// Q: wires the one selected node to the nearest free pin of another one
	void			ConnectNearest	();
	// Alt+D: the node stays put and the chain closes over it (BlueprintAssist's Alt+D)
	void			BypassSelected	();
	// the single gate every new edge passes: false + `why` when the validator
	// would reject it on the spot (E007)
	bool			CanLink			(LPCSTR from, LPCSTR to, xr_string& why) const;
	LPCSTR			Status			() const { return m_Status.c_str(); }	// last refusal / result

	// world <-> screen of the last Draw
	ImVec2			ToScreen		(const ImVec2& w) const;
	ImVec2			ToWorld			(const ImVec2& s) const;
	float			Zoom			() const				{ return ZoomOf(m_Doc->zoom_idx); }
	bool			Hovered			() const				{ return m_Hovered; }

private:
	struct SNodeGeom
	{
		ImVec2	pos, size;			// world, top-left + size
		bool	trigger;
		float	strip_h;			// chip strip height (0 for triggers)
		int		enter_n, exit_n;	// chip counts
		xr_vector<xr_string> pins;	// output pins in draw order
		int		declared;			// pins[0..declared) come from the catalog, the rest only from the file
	};
	struct SLinkGeom
	{
		int			from;			// node index
		xr_string	pin;
		int			to;
		ImVec2		p0, p1, p2, p3;	// screen
	};

	NqDoc*			m_Doc;
	ImVec2			m_Origin;		// screen top-left of the canvas rect
	ImVec2			m_Size;
	bool			m_Hovered;
	bool			m_ShowMinimap;
	bool			m_MinimapHovered;
	bool			m_MinimapPanning;
	ImVec2			m_MinimapMin, m_MinimapMax;
	ImVec2			m_MinimapWorldMin, m_MinimapWorldMax;
	float			m_MinimapScale;

	// interaction
	bool			m_Panning;
	bool			m_RmbMoved;
	ImVec2			m_RmbDown;
	bool			m_Dragging;
	bool			m_DragMoved;
	ImVec2			m_DragStartMouse;
	xr_vector<std::pair<xr_string, ImVec2> > m_DragStart;	// node id -> start pos (world)
	bool			m_Marquee;
	ImVec2			m_MarqueeFrom;	// screen
	bool			m_Linking;
	xr_string		m_LinkFrom, m_LinkPin;
	xr_string		m_Status;		// last refusal / result of a link gesture (drawn briefly)
	double			m_StatusAt;
	int				m_HotNode;		// node whose pin the cursor is on (-1 none)
	int				m_HotPin;		// output pin index, or -1 for the input pin
	int				m_HotLink;		// link under the cursor (-1 none) - lit, Alt+LMB or D removes it
	// pending "create node and connect" after dropping a link on empty space
	bool			m_PendingLink;
	ImVec2			m_MenuWorld;	// world position for node creation from menus
	xr_string		m_CtxNode;		// node under the context menu
	xr_string		m_CtxLinkFrom, m_CtxLinkPin, m_CtxLinkTo;
	char			m_Filter[64];	// add-node menu filter
	int				m_FilterSel;	// highlighted row of the filtered list (keyboard)
	xr_string		m_ChipDragNode, m_ChipDragSlot;
	int				m_ChipDragIndex;
	// a press on a chip only nominates it; the move starts once the mouse has
	// actually travelled (ImGui's own drag threshold), so a click stays a click
	bool			m_ChipDragging;
	bool			m_WantFrameAll, m_WantFrameSel, m_WantRename, m_WantFocusAction, m_OpenAddAction;
	int				m_WantZoom;		// zoom asked for explicitly (-1 = none); applied after framing
	xr_vector<SViewState> m_BackHistory, m_ForwardHistory;
	xr_vector<xr_string> m_Bookmarks;
	xr_flat_hash_map<xr_string, u8> m_BookmarkLookup;
	bool			m_ReplayingHistory;
	bool			m_HistoryReady;
	int				m_HistoryBatchDepth;
	bool			m_HistoryBatchRemembered;
	double			m_LastWheelHistory;
	bool			m_WheelZooming;
	bool			m_MiddlePanning;
	void			CancelFraming	()	{ m_WantFrameAll = m_WantFrameSel = false; }
	bool			Measured		() const { return m_Size.x > 32.f && m_Size.y > 32.f; }
	u32				m_ReachRevision;
	xr_vector<bool>	m_Reachable;

	// render caches
	xr_vector<SNodeGeom> m_Geom;
	xr_vector<SLinkGeom> m_Links;
	u32				m_GeomRevision;
	u32				m_GeomCatalogGeneration;
	u32				m_LinkRevision;
	u32				m_LinkCatalogGeneration;
	ImVec2				m_LinkOrigin;
	ImVec2				m_LinkSize;
	float				m_LinkViewX;
	float				m_LinkViewY;
	int					m_LinkZoom;

	void			BuildGeometry	();
	void			BuildLinks		();
	const SNodeGeom* GeomOf			(LPCSTR id, int* index = 0) const;
	ImVec2			InputPin		(const SNodeGeom& g) const;			// world
	ImVec2			OutputPin		(const SNodeGeom& g, int pin) const;	// world
	// DPI factor every raw pixel constant of the canvas is measured in
	float			UiScale			() const;
	// how far from a pin the mouse may be and still grab it (screen pixels)
	float			PinGrab			() const;
	bool			NearPin			(const ImVec2& screen, const ImVec2& c, float inward, float& d2) const;
	int				HitNode			(const ImVec2& screen) const;			// index or -1
	// nearest node within the grab radius of its box (`except` skipped), or -1
	int				HitNodeNear		(const ImVec2& screen, int except = -1) const;
	// nearest output pin of one node / of every node; `d2` gets its distance
	int				HitPin			(int node, const ImVec2& screen, float* d2 = 0) const;
	bool			HitAnyPin		(const ImVec2& screen, int& node, int& pin) const;
	bool			HitChip			(int node, const ImVec2& screen, xr_string& slot, int& index, bool& plus) const;
	int				HitLink			(const ImVec2& screen) const;			// link index or -1
	bool			MenuOpen		() const;								// a popup of this canvas is up
	void			UpdateHot		();
	void			DrawGrid		(ImDrawList* dl);
	void			DrawLinks		(ImDrawList* dl);
	void			DrawNode		(ImDrawList* dl, int index);
	void			DrawChipStrip	(ImDrawList* dl, int index, LPCSTR slot, const ImVec2& tl, float w);
	void			DrawMarquee		(ImDrawList* dl);
	void			DrawLinking		(ImDrawList* dl);
	void			DrawMinimap		(ImDrawList* dl);
	void			HandleInput		();
	void			HandleKeys		();
	void			OpenContextMenus();
	void			DrawContextMenus();
	void			DrawAddNodeMenu	(LPCSTR popup);
	// filter box + list of the search popups; the chosen kind, or 0
	const NqCatalog::SKind* PickKind(LPCSTR hint, u32 use_mask);
	void			InsertNodes		(xr_vector<SNqNode>& in, const ImVec2& place, bool anchor);
	void			CreateNode		(const NqCatalog::SKind& k, const ImVec2& world, bool connect_pending);
	void			SetStatus		(LPCSTR text);
	void			DeleteHotLink	();										// Alt+LMB and D share this
	void			SelectOnly		(LPCSTR id);
	bool			IsSelected		(LPCSTR id) const;
	void			ToggleSelected	(LPCSTR id);
	void			EndDrag			();
	void			EndLink			();
	void			EndMarquee		();
	void			EnsureReachable	();
	xr_string		NodeTitle		(const SNqNode& n) const;
	xr_string		NodeSummary		(const SNqNode& n) const;
	ImU32			NodeColor		(const SNqNode& n, bool selected) const;
	int				Severity		(LPCSTR node_id) const;		// 0 none, 1 warning, 2 error
	void			ProblemTooltip	(LPCSTR node_id) const;
	void			ChipTooltip		(const SNqAction& a) const;
	void			FrameRect		(const ImVec2& wmin, const ImVec2& wmax);
	void			RememberView	();
	void			ApplyView		(const SViewState& state);
	void			PruneTransient	();
	void			ToggleSelectedBookmarks();
	ImVec2			MinimapToWorld	(const ImVec2& screen) const;
};
