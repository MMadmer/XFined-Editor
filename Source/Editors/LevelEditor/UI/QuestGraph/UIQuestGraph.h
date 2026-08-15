#pragma once

// NQ - the quest graph editor tab (docs/NQ_ARCHITECTURE.md par. 13.7): one ImGui
// window per open document, docked as a tab in the centre like an asset editor.
// Toolbar on top, canvas | inspector split below, problems strip at the bottom.
// UIQuestGraph is the registry (Open/Update/CloseAll) pumped from
// CLevelMain::OnDrawUI; the documents themselves live in NqDocs (XrECore) so MCP
// and the UI share them.

#include "../../../XrECore/Editor/Nq/NqDoc.h"

class NqCanvas;
class NqInspector;

class UIQuestGraphWindow : public XrUI
{
public:
	explicit		UIQuestGraphWindow	(NqDoc* doc);
	virtual			~UIQuestGraphWindow	();
	virtual void	Draw				();

	NqDoc*			Doc					()	{ return m_Doc; }
	NqCanvas*		Canvas				()	{ return m_Canvas; }
	void			Focus				()	{ m_Focus = true; }
	// close request from the tab's X or CloseAll: dirty documents ask first
	void			RequestClose		(bool discard);
	bool			WantsClose			() const { return m_CloseNow; }

private:
	NqDoc*			m_Doc;
	NqCanvas*		m_Canvas;
	NqInspector*	m_Inspector;
	float			m_Split;			// inspector width in pixels
	bool			m_Focus;
	bool			m_AskClose;			// modal "unsaved changes"
	bool			m_CloseNow;
	bool			m_ShowProblems;
	xr_string		m_Message;			// last save/reload result

	void			DrawToolbar			();
	void			DrawProblems		();
	void			DrawClosePrompt		();
	void			Save				();
	xr_string		Title				() const;
};

namespace UIQuestGraph
{
	// opens (or focuses) the tab for a project-relative or absolute path;
	// false + err when the file cannot be read (err = 0 -> message box)
	bool			Open				(LPCSTR path, xr_string* err);
	// draws every open tab; call from CLevelMain::OnDrawUI
	void			Update				();
	// closes every tab (dirty ones ask unless `discard`)
	void			CloseAll			(bool discard);
	int				Count				();
	UIQuestGraphWindow* Find			(LPCSTR path);
	// MCP quest_view: {"path","frame":"all"|"<node>","zoom_level":n,"center":{x,y}}
	void			McpView				(LPCSTR raw, xr_string& out);
	// forget every tab when the project changes
	void			OnProjectChanged	();
}
