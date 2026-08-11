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
	// GPU the render device is created on, by adapter description; empty means
	// the system default. Applied on the next start - D3D9 cannot move a live
	// device to another adapter.
	xr_string GpuAdapter;
};