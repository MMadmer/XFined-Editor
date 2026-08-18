#pragma once

// NQ - the quest graph editor tab (docs/NQ_ARCHITECTURE.md par. 13.7): one ImGui
// window per open document, docked as a tab in the centre like an asset editor.
// Toolbar on top, canvas | inspector split below, problems strip at the bottom.
// UIQuestGraph is the registry (Open/Update/CloseAll) pumped from
// CLevelMain::OnDrawUI; the documents themselves live in NqDocs (XrECore) so MCP
// and the UI share them.

#include "../../../XrECore/Editor/Nq/NqDoc.h"
#include "../../../XrECore/Editor/Nq/NqProjectIndex.h"

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
	void			McpFind				(LPCSTR raw, xr_string& out);
	void			McpBookmarks		(LPCSTR raw, xr_string& out);
	void			McpHistory			(LPCSTR raw, xr_string& out);
	void			McpMinimap			(LPCSTR raw, xr_string& out);

private:
	struct SFindResult
	{
		xr_string	node;
		xr_string	kind;
		xr_string	title;
		xr_string	match;
	};

	NqDoc*			m_Doc;
	NqCanvas*		m_Canvas;
	NqInspector*	m_Inspector;
	// inspector width as a fraction of the body, not pixels: the tab is resized
	// and rescaled far more often than the split is dragged
	float			m_SplitFrac;
	bool			m_Focus;
	bool			m_AskClose;			// modal "unsaved changes"
	bool			m_CloseNow;
	bool			m_ShowProblems;
	xr_string		m_Message;			// last save/reload result
	bool			m_ShowFind;
	bool			m_FocusFind;
	char			m_Find[256];
	int				m_FindIndex;
	u32				m_FindRevision;
	u32				m_FindCatalogGeneration;
	xr_vector<SFindResult> m_FindResults;
	bool			m_OpenProjectFind;
	char			m_ProjectFind[256];
	xr_vector<NqProjectIndex::SFindResult>	m_ProjectFindResults;
	xr_vector<NqProjectIndex::SDiagnostic>	m_ProjectFindDiagnostics;
	xr_string		m_ProjectFindError;
	bool			m_ProjectFindComplete;
	u32			m_ProjectFindGeneration;
	u64			m_ProjectFindFingerprint;

	void			DrawToolbar			();
	void			DrawFindBar			();
	void			RefreshProjectFind	();
	void			DrawProjectFind		();
	bool			NavigateProjectFind	(int index);
	void			RefreshFind			(bool force = false);
	bool			NavigateFind		(int index);
	void			SetFindQuery		(LPCSTR query);
	void			AppendFindState		(xr_string& out, int limit);
	void			AppendViewState		(xr_string& out);
	void			DrawSplitter		(float body_w, float body_h);
	void			DrawProblems		();
	void			DrawClosePrompt		();
	void			Save				();
	// inspector width in pixels for a body `body_w` wide, both panes kept usable
	float			SplitWidth			(float body_w) const;
	// vertical space the problems strip needs this frame (header + list)
	float			ProblemsHeight		() const;
	float			ProblemListHeight	() const;
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
	void			McpClose			(LPCSTR raw, xr_string& out);
	void			McpView				(LPCSTR raw, xr_string& out);
	void			McpFind				(LPCSTR raw, xr_string& out);
	void			McpBookmarks		(LPCSTR raw, xr_string& out);
	void			McpHistory			(LPCSTR raw, xr_string& out);
	void			McpMinimap			(LPCSTR raw, xr_string& out);
	// forget every tab when the project changes
	void			OnProjectChanged	();
}
