#pragma once

// Unreal-style World Outliner: the whole scene as one tree, grouped by object
// tool class. Unlike UIObjectList (flat, current tool only) this panel shows
// every class at once and keeps its selection in sync with the viewport both
// ways - rows read Selected() live, clicks drive the scene selection.
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

private:
	struct SGroup
	{
		ObjClassID					cls;
		xr_string					name;		// tool ClassName()
		xr_vector<CCustomObject*>	objects;	// the scene list, flattened
		xr_vector<int>				shown;		// indices into objects, filter result
	};

	static UIWorldOutliner*			Form;

	xr_vector<SGroup>				m_Groups;
	u32								m_Signature;	// per-class counts, cheap change probe
	ObjClassID						m_TargetClass;	// LTools target the cache was built for
	int								m_TotalObjects;
	bool							m_Dirty;

	// filter state the `shown` lists were built with
	xr_string						m_FilterApplied;
	bool							m_SelectedOnlyApplied;

	char							m_Filter[128];
	bool							m_SelectedOnly;

	// shift-range anchor, the last plainly clicked row
	ObjClassID						m_AnchorClass;
	int								m_AnchorRow;

	// auto-scroll: a selection change that did NOT come from this panel brings
	// the first selected row into view. The target pointer lives one Draw() -
	// it is resolved and consumed within the same frame's scene walk, so it
	// can never dangle across scene edits.
	u32								m_SelSignature;
	CCustomObject*					m_ScrollTarget;
	bool							m_SkipNextScroll;

	// scene edits are deferred: the tree must not mutate while it iterates
	CCustomObject*					m_PendingDelete;
	CCustomObject*					m_PendingRename;
	bool							m_OpenRenamePopup;
	char							m_RenameBuf[256];
	char							m_RenameError[128];

	void			Rebuild				();
	u32				SceneSignature		();
	void			ApplyFilter			();
	bool			Filtering			() const { return (0 != m_Filter[0]) || m_SelectedOnly; }
	void			DrawGroup			(SGroup& g);
	void			DrawRow				(SGroup& g, int row, CCustomObject* obj);
	void			DrawRenamePopup		();
	bool			ApplyRename			();
	void			RunPending			();
	// one scene walk per frame: count, change signature, first selected object
	int				ScanSelection		(u32& sig, CCustomObject*& first) const;

	// plain viewport-pick semantics: drop the selection, take this object
	static void		PickObject			(CCustomObject* obj);
	static void		FocusObject			(CCustomObject* obj);
};
