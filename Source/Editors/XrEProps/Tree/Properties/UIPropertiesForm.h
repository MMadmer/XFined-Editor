#pragma once
class XREPROPS_API UIPropertiesForm :public XrUI
{
	friend class UIPropertiesItem;
public:
	UIPropertiesForm();
	virtual ~UIPropertiesForm();
	virtual void Draw();
	void AssignItems(PropItemVec& items);
	PropItem* FindItem(const char* path);
	PropItem* FindItemOfName(shared_str name);
	void ClearProperties();
	void SetFilter(LPCSTR filter);
	LPCSTR GetFilter() const { return m_Filter; }
	void ExpandAll();
	void CollapseAll();
	u32 VisiblePropertyCount() const { return m_VisiblePropertyCount; }
	u32 PropertyCount() const { return m_PropertyCount; }
	IC void SetReadOnly(bool enable) { m_Flags.set(plReadOnly, enable); }
	IC bool IsModified() { return m_bModified;}
	IC bool Empty() { return m_Items.size() == 0; }
	void SetModifiedEvent(TOnModifiedEvent modif = 0) { OnModifiedEvent = modif; }
public:
	enum {
		plReadOnly = (1 << 0),
	};
	Flags32 m_Flags;
	IC bool IsReadOnly()const { return m_Flags.is(plReadOnly); }
private:
	PropItemVec m_Items;
	PropItem* m_EditChooseValue;
	PropItem* m_EditTextureValue;
	PropItem* m_EditShortcutValue;
private:
	TOnModifiedEvent 	OnModifiedEvent;
private:
	PropItem* m_EditTextValue;
	char* m_EditTextValueData;
	int m_EditTextValueDataSize;
	void DrawEditText();
	int  DrawEditText_Callback(ImGuiInputTextCallbackData* data);
private:
	GameTypeChooser m_EditGameTypeChooser;
	PropItem* m_EditGameTypeValue;
	void DrawEditGameType();
	bool m_bModified;
	void Modified() { m_bModified = true; if (!OnModifiedEvent.empty()) OnModifiedEvent(); }
private:
	void RefreshFilter();
	bool IsFiltering() const { return m_Filter[0] != 0; }
	bool RestoreExpansion(LPCSTR path, bool fallback) const;
	void RememberExpansion(LPCSTR path, bool expanded);
	char m_Filter[256];
	u32 m_VisiblePropertyCount;
	u32 m_PropertyCount;
	bool m_ExpandByDefault;
	xr_flat_hash_map<xr_string, bool> m_ExpansionState;

	UIPropertiesItem m_Root;

/*	virtual void DrawNode(Node* pNode);
	virtual void DrawItem(Node* pNode);
	virtual void DrawItem(const char*name,PropItem* Node);
	virtual bool IsDrawFloder(Node* Node);
	virtual void DrawAfterFloderNode(bool is_open, Node* Node = 0);
private:
	
	Node m_GeneralNode;


private:
	void RemoveMixed(Node* Node);*/
};

