#pragma once
class XREPROPS_API UIPropertiesItem :public UITreeItem
{
	friend class UIPropertiesForm;
public:
	UIPropertiesItem(shared_str Name, UIPropertiesForm* PropertiesFrom);
	virtual ~UIPropertiesItem();
	PropItem* PItem;
	UIPropertiesForm* PropertiesFrom;
	void Draw();
	void DrawRoot();
	void DrawItem();
	void DrawProp();
protected:
	virtual UITreeItem* CreateItem(shared_str Name);
private:
	void RemoveMixed();
	bool RefreshFilter(LPCSTR filter, const xr_string& parentPath, bool parentMatches, u32& visibleCount, u32& totalCount);
	void SetExpandedRecursive(bool expanded);
	void Reset(bool expanded);
	bool m_FilterVisible;
	bool m_Expanded;
	xr_string m_Path;
};
