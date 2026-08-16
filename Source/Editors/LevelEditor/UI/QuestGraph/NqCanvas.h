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
	// framing needs the canvas rect: it runs at once when a Draw has already
	// measured it, otherwise it waits for the next one
	void			RequestFrameAll	();
	void			RequestFrameSel	();
	void			RequestFrameNode(LPCSTR id);

	// the inspector wants the id field focused (F2 / rename)
	bool			TakeRenameRequest()						{ bool r = m_WantRename; m_WantRename = false; return r; }
	// the inspector wants the action section focused (chip double click)
	bool			TakeFocusAction	()						{ bool r = m_WantFocusAction; m_WantFocusAction = false; return r; }

	// clipboard: selected nodes as a Lua fragment
	void			CopySelection	();
	void			PasteClipboard	();
	void			DeleteSelection	();
	void			SelectAll		();
	void			DuplicateSelection();

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
	xr_string		m_Refused;		// why the last link drop was refused (drawn briefly)
	double			m_RefusedAt;
	// pending "create node and connect" after dropping a link on empty space
	bool			m_PendingLink;
	ImVec2			m_MenuWorld;	// world position for node creation from menus
	xr_string		m_CtxNode;		// node under the context menu
	xr_string		m_CtxLinkFrom, m_CtxLinkPin, m_CtxLinkTo;
	char			m_Filter[64];	// add-node menu filter
	int				m_FilterSel;	// highlighted row of the filtered list (keyboard)
	xr_string		m_ChipDragNode, m_ChipDragSlot;
	int				m_ChipDragIndex;
	bool			m_WantFrameAll, m_WantFrameSel, m_WantRename, m_WantFocusAction, m_OpenAddAction;
	xr_string		m_WantFrameNode;
	int				m_WantZoom;		// zoom asked for explicitly (-1 = none); applied after framing
	void			CancelFraming	()	{ m_WantFrameAll = m_WantFrameSel = false; m_WantFrameNode.clear(); }
	bool			Measured		() const { return m_Size.x > 32.f && m_Size.y > 32.f; }
	u32				m_ReachRevision;
	xr_vector<bool>	m_Reachable;

	// per-frame caches
	xr_vector<SNodeGeom> m_Geom;
	xr_vector<SLinkGeom> m_Links;

	void			BuildGeometry	();
	void			BuildLinks		();
	const SNodeGeom* GeomOf			(LPCSTR id, int* index = 0) const;
	ImVec2			InputPin		(const SNodeGeom& g) const;			// world
	ImVec2			OutputPin		(const SNodeGeom& g, int pin) const;	// world
	int				HitNode			(const ImVec2& screen) const;			// index or -1
	int				HitPin			(int node, const ImVec2& screen) const;	// output pin index or -1
	bool			HitChip			(int node, const ImVec2& screen, xr_string& slot, int& index, bool& plus) const;
	int				HitLink			(const ImVec2& screen) const;			// link index or -1
	void			DrawGrid		(ImDrawList* dl);
	void			DrawLinks		(ImDrawList* dl);
	void			DrawNode		(ImDrawList* dl, int index);
	void			DrawChipStrip	(ImDrawList* dl, int index, LPCSTR slot, const ImVec2& tl, float w);
	void			DrawMarquee		(ImDrawList* dl);
	void			DrawLinking		(ImDrawList* dl);
	void			HandleInput		();
	void			HandleKeys		();
	void			OpenContextMenus();
	void			DrawContextMenus();
	void			DrawAddNodeMenu	(LPCSTR popup);
	// filter box + list of the search popups; the chosen kind, or 0
	const NqCatalog::SKind* PickKind(LPCSTR hint, u32 use_mask);
	void			InsertNodes		(xr_vector<SNqNode>& in, const ImVec2& place, bool anchor);
	void			CreateNode		(const NqCatalog::SKind& k, const ImVec2& world, bool connect_pending);
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
};
