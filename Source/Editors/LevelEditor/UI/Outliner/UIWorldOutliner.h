#pragma once

// Unreal-style World Outliner: the whole scene grouped by object tool class or
// shown as a view-only owner hierarchy. Unlike UIObjectList (flat, current tool
// only) this panel shows every class at once and keeps its selection in sync
// with the viewport both ways - rows read Selected() live, clicks drive it.
//
// The scene stores objects in xr_list, so rows cannot be indexed directly.
// Per-group pointer vectors are cached and rebuilt only when the scene really
// changed (per-class object counts + an explicit dirty flag).

class CCustomObject;

class UIWorldOutliner : public XrUI
{
public:
					UIWorldOutliner		();
	virtual			~UIWorldOutliner	();
	virtual void	Draw				();

	static void		Update				();
	static void		Show				();
	static void		Close				();
	static IC bool	IsOpen				() { return !!Form; }

	// invalidates the cache; the next frame rebuilds it
	static void		Refresh				();

	// MCP: drive the search box and the type funnel from outside. `text` null
	// keeps the current search, `selected_only` < 0 keeps the toggle, `types`
	// null keeps the funnel - an empty string shows every type, otherwise it is
	// a ';'-separated list of the tool class names to SHOW. Unknown names are
	// reported rather than silently dropped.
	static bool		McpSetFilter		(LPCSTR text, int selected_only, LPCSTR types,
										 LPCSTR visibility, xr_string& err);
	// MCP: what the panel is filtering by right now, and how much survives it
	static void		McpGetFilter		(xr_string& text, bool& selected_only,
										 xr_string& hidden, xr_string& visibility,
										 int& shown, int& total);

private:
	struct SGroup
	{
		ObjClassID					cls;
		xr_string					name;		// tool ClassName()
		xr_vector<CCustomObject*>	objects;	// the scene list, flattened
		xr_vector<int>				shown;		// indices into objects, filter result
		xr_vector<int>				hierarchy;	// parallel object-to-node lookup
	};

	struct SHierarchyNode
	{
		CCustomObject*				object;
		int						group;
		int						parent;
		xr_vector<int>				children;
		bool						direct;
		bool						included;
		bool						expanded;
	};

	struct SHierarchyRow
	{
		int						node;
		int						depth;
	};

	struct SHierarchyIdentity
	{
		ObjClassID					cls;
		shared_str					name;
	};

	static UIWorldOutliner*			Form;

	xr_vector<SGroup>				m_Groups;
	u32								m_Signature;	// per-class counts, cheap change probe
	ObjClassID						m_TargetClass;	// LTools target the cache was built for
	int								m_TotalObjects;
	bool							m_Dirty;
	xr_vector<SHierarchyNode>		m_Hierarchy;
	xr_vector<int>					m_HierarchyRoots;
	xr_vector<SHierarchyRow>		m_HierarchyRows;
	std::map<xr_string, SHierarchyIdentity> m_HierarchyExpanded;
	bool							m_HierarchyView;
	bool							m_HierarchyDirty;
	bool							m_HierarchyRowsDirty;
	int								m_HierarchySort;

	// filter state the `shown` lists were built with
	xr_string						m_FilterApplied;
	bool							m_SelectedOnlyApplied;
	int								m_VisibilityApplied;
	int								m_SortApplied;

	char							m_Filter[128];
	bool							m_SelectedOnly;
	// Visibility is a view-only IXRay filter: 0 = all, 1 = visible, 2 = hidden.
	int								m_Visibility;
	// The ImGui table owns persistence; 0 = scene order, 1 = A-Z, 2 = Z-A.
	int								m_Sort;

	// Unreal's search-box grammar (FTextFilterExpressionEvaluator in its basic
	// mode): spaces separate terms, EVERY plain term has to match, a term
	// written -like_this must not, and "quoted words" keep their spaces. Parsed
	// when the text changes rather than per row.
	struct STerm
	{
		xr_string					text;		// lowercased at parse time
		bool						exclude;
	};
	xr_vector<STerm>				m_Terms;
	// the funnel next to the search box: object types switched off by hand
	xr_vector<ObjClassID>			m_HiddenClasses;

	// Flat and hierarchy ranges keep independent stable identities.
	ObjClassID						m_AnchorClass;
	shared_str						m_AnchorName;
	SHierarchyIdentity				m_HierarchyAnchor;

	// auto-scroll: a selection change that did NOT come from this panel brings
	// the last selected row into view - last in scene order, since nothing
	// records the order picks were actually made in. The target pointer lives
	// one Draw() - it is resolved and consumed within the same frame's scene
	// walk, so it can never dangle across scene edits.
	u32								m_SelSignature;
	CCustomObject*					m_ScrollTarget;
	bool							m_SkipNextScroll;

	// scene edits are deferred: the tree must not mutate while it iterates
	shared_str						m_DeleteName;
	shared_str						m_PendingDeleteName;
	shared_str						m_RenameOriginal;
	bool							m_OpenDeletePopup;
	bool							m_OpenRenamePopup;
	char							m_RenameBuf[256];
	char							m_RenameError[128];

	void			Rebuild				();
	u32				SceneSignature		();
	void			ApplyFilter			();
	// splits the search box into terms; only run when the text changes
	void			ParseFilter			();
	bool			NameMatches			(LPCSTR name) const;
	bool			ClassHidden			(ObjClassID cls) const;
	bool			AnyTypeHidden		() const;
	// the funnel popup: which object types the tree shows
	void			DrawFilterMenu		();
	bool			Filtering			() const { return !m_Terms.empty() || m_SelectedOnly || m_Visibility; }
	bool			HierarchyFiltering	() const { return Filtering() || AnyTypeHidden(); }
	void			DrawGroup			(SGroup& g);
	void			DrawFlatRow			(SGroup& g, int row, CCustomObject* obj);
	void			DrawHierarchy		();
	void			DrawHierarchyRow	(int row, const SHierarchyRow& item);
	void			DrawObjectContext	(SGroup& g, CCustomObject* obj);
	void			HandleRowSelection	(SGroup& g, int row, CCustomObject* obj);
	void			HandleHierarchySelection(int row, CCustomObject* obj);
	void			BuildHierarchy		();
	void			InvalidateHierarchy	();
	void			ApplyHierarchyFilter();
	void			BuildHierarchyRows	();
	void			SetHierarchyExpanded(int node, bool expanded);
	void			RevealHierarchyPath	(CCustomObject* obj);
	void			UpdateHierarchyIdentity(CCustomObject* obj, LPCSTR new_name);
	static xr_string HierarchyIdentityKey(ObjClassID cls, LPCSTR name);
	// Unreal's SListView::ScrollIntoView rule: a row already on screen is left
	// alone, one off screen is pulled to the NEAREST edge. Never recentres -
	// that is what makes clicking a visible row feel like the list ran away.
	// Takes the row's content-space top, so a clipped row needs no drawing.
	static void		ScrollRowIntoView	(float row_top, float row_h);
	void			DrawRenamePopup		();
	void			DrawDeletePopup		();
	bool			ApplyRename			();
	void			RunPending			();
	// one scene walk per frame: count, change signature, last selected object
	int				ScanSelection		(u32& sig, CCustomObject*& last) const;

	// plain viewport-pick semantics: drop the selection, take this object
	static void		PickObject			(CCustomObject* obj);
	static void		FocusObject			(CCustomObject* obj);
	static int		NaturalCompare		(LPCSTR lhs, LPCSTR rhs);
};
