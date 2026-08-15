#pragma once

// NQ - the details panel of a quest document (docs/NQ_ARCHITECTURE.md par. 13.9):
// upper section = the selected node (or the quest when nothing is selected),
// lower section = the selected on_enter/on_exit action. Widgets are generated
// from the catalog parameter types; edits are staged on a copy while a widget
// is active and committed to NqDoc (one undo step) when the interaction ends.

#include "../../../XrECore/Editor/Nq/NqDoc.h"
#include "../../../XrECore/Editor/Nq/NqCatalog.h"

class NqInspector
{
public:
	explicit		NqInspector		(NqDoc* doc);

	// draws both sections into the current window (child regions)
	void			Draw			(bool focus_rename, bool focus_action);

private:
	NqDoc*			m_Doc;
	// staged node copy
	SNqNode			m_Node;
	xr_string		m_NodeId;
	u32				m_NodeRev;
	bool			m_NodeDirty;
	// staged quest header copy
	SNqQuest		m_Quest;
	u32				m_QuestRev;
	bool			m_QuestDirty;
	xr_string		m_LuaCheck;		// last syntax check result
	bool			m_FocusRename, m_FocusAction;
	char			m_Search[64];	// picker popup filter

	void			SyncNode		();
	void			CommitNode		();
	void			SyncQuest		();
	void			CommitQuest		();

	void			DrawQuestSection();
	void			DrawNodeSection	();
	void			DrawActionSection();

	// generic parameter editors; every one returns true when the value changed
	bool			DrawParams		(const NqCatalog::SKind* k, SNqValue& params, LPCSTR id_prefix);
	bool			DrawParam		(const NqCatalog::SParam& p, SNqValue& params, LPCSTR id_prefix);
	bool			DrawTyped		(LPCSTR type, const NqCatalog::SParam* p, LPCSTR label, SNqValue& v);
	bool			DrawText		(LPCSTR label, SNqValue& v, bool multiline);
	bool			DrawPicked		(LPCSTR label, LPCSTR type, xr_string& s);
	bool			DrawDuration	(LPCSTR label, SNqValue& v);
	bool			DrawNpcRef		(LPCSTR label, SNqValue& v, bool with_smart, bool with_spawn);
	bool			DrawPlace		(LPCSTR label, SNqValue& v);
	bool			DrawSpawnSpec	(LPCSTR label, SNqValue& v);
	bool			DrawCases		(LPCSTR label, SNqValue& v, bool weights);
	bool			DrawCondList	(xr_vector<SNqCond>& conds, LPCSTR id_prefix, int depth);
	bool			DrawCondValue	(LPCSTR label, SNqValue& v, int depth);		// cond_list inside params
	bool			DrawLua			(LPCSTR label, SNqValue& v);
	bool			DrawScalarValue	(LPCSTR label, SNqValue& v);
	bool			DrawRaw			(LPCSTR label, SNqValue& v);
	bool			DrawKindCombo	(LPCSTR label, u32 use_mask, xr_string& kind);
	bool			PickerPopup		(LPCSTR popup, LPCSTR type, xr_string& out);
};
