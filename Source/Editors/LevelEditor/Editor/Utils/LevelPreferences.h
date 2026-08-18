#pragma once

class CLevelPreferences: public CCustomPreferences
{
	typedef CCustomPreferences			inherited;
	void 	OnEnabledChange		(PropValue* prop);
	void 	OnReadonlyChange	(PropValue* prop);
protected:
    virtual void 	Load				(CInifile*);
    virtual void 	Save				(CInifile*);
public:
    virtual void	FillProp          	(PropItemVec& items);
	bool OpenObjectList;
	bool OpenProperties;
	bool OpenWorldProperties;
	bool OpenContentBrowser;
	bool OpenWorldOutliner;
	// content browser folder-tree width in pixels, dragged by its splitter
	u32  ContentBrowserTreeWidth;
	// quest graph inspector width, in permille of the tab body. A fraction and not
	// pixels: the tab is resized and rescaled far oftener than the split is dragged
	u32  QuestInspectorSplit;
	// height of the variables pane inside that inspector, permille of the column
	u32  QuestVarsSplit;
	BOOL RecoveryEnabled = TRUE;
	u32 RecoveryIntervalMinutes = 10;
	u32 RecoveryInteractionDelaySeconds = 15;
	u32 RecoveryRetention = 10;
	// GPU the render device is created on, by adapter description; empty means
	// the system default. Applied on the next start - D3D9 cannot move a live
	// device to another adapter.
	xr_string GpuAdapter;
};
