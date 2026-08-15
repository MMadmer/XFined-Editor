#pragma once

// NQ - an open quest document (docs/NQ_ARCHITECTURE.md par. 13.4): the quest, its
// file, dirty state, undo/redo ring, current problems, and the operation set
// shared by the MCP quest_apply and the (Phase 4) canvas/inspector. UI-free, so
// MCP works headless; the registry (NqDocs) is keyed by normalized path.

#include "NqAsset.h"
#include "NqValidate.h"

class ECORE_API NqDoc
{
public:
	xr_string				path;			// absolute, normalized (backslashes)
	SNqQuest				quest;
	bool					dirty;
	xr_vector<SNqProblem>	problems;		// last Validate()
	// view state (canvas), not part of the file
	float					view_cx, view_cy;
	int						zoom_idx;
	xr_vector<xr_string>	selection;		// node ids
	xr_string				sel_slot;		// "enter:2" / "exit:0" / "" - inspector focus
	// generation counter: bumps on every change (UI can cache against it)
	u32						revision;

	NqDoc();

	// -- file --------------------------------------------------------------------
	// reads `path`; false + err when the file cannot be read or does not parse
	// (the document keeps its previous content on failure)
	bool			Load			(xr_string& err);
	// replaces the content from text (quest_write). Parse/shape errors leave the
	// document unchanged. Records an undo snapshot.
	bool			SetFromLua		(const xr_string& text, xr_string& err);
	// canonical write; refuses when the file changed on disk since Load/Save
	// unless `force` (err = "modified externally")
	bool			Save			(bool force, xr_string& err);
	// teardown write: the document is about to be destroyed, so neither the
	// "belongs to the active project" gate (the project may already be gone) nor
	// the "changed on disk" one may stop it. A file that changed under us keeps
	// its disk copy and the edits land in a sibling ".recovered"; `written` gets
	// the path that was actually produced.
	bool			SaveRescue		(xr_string& written, xr_string& err);
	bool			Reload			(xr_string& err);
	bool			FileExists		() const;
	bool			ModifiedExternally() const;
	// canonical text of the current content
	xr_string		Text			() const;
	xr_string		Outline			() const;

	// -- editing -------------------------------------------------------------------
	// one operation (a Lua table {op = "...", ...}, see quest_apply in
	// docs/NQ_ARCHITECTURE.md par. 13.11); false + err leaves the document unchanged
	bool			Apply			(const SNqValue& op, xr_string& err);
	// list of operations, all-or-nothing; `applied` = count on success
	bool			ApplyOps		(const SNqValue& ops, xr_string& err, int& applied);
	// typed shortcuts used by the UI (each records one undo snapshot)
	bool			AddNode			(const SNqNode& n, xr_string& err);
	bool			RemoveNodes		(const xr_vector<xr_string>& ids, xr_string& err);
	bool			RenameNode		(LPCSTR id, LPCSTR new_id, xr_string& err);
	bool			Connect			(LPCSTR from, LPCSTR pin, LPCSTR to, xr_string& err);
	bool			Disconnect		(LPCSTR from, LPCSTR pin, LPCSTR to, xr_string& err);
	bool			SetPos			(LPCSTR id, float x, float y, bool snapshot);
	// mutate through a callback: Snapshot(); fn(quest); Changed()
	template <class F> void Edit(F fn) { Snapshot(); fn(quest); Changed(); }

	// -- undo ----------------------------------------------------------------------
	void			Snapshot		();		// push current quest to undo, clear redo
	bool			Undo			();
	bool			Redo			();
	int				UndoDepth		() const { return (int)m_Undo.size(); }
	int				RedoDepth		() const { return (int)m_Redo.size(); }
	// marks dirty, bumps revision, revalidates
	void			Changed			();

	// -- validation and layout ---------------------------------------------------------
	void			Validate		();
	int				ErrorCount		() const { return NqValidate::CountErrors(problems); }
	int				WarningCount	() const { return NqValidate::CountWarnings(problems); }
	// auto layout (all nodes, or only those without pos); records a snapshot
	// when something moved
	int				Layout			(bool only_missing);
	// unique node id derived from `base` ("step", "step2", ...)
	xr_string		FreeNodeId		(LPCSTR base) const;

private:
	xr_vector<SNqQuest>	m_Undo;
	xr_vector<SNqQuest>	m_Redo;
	u64					m_FileTime;		// last-write time seen at Load/Save (0 = new file)
	// the file held constructs the reader could not represent (E003) and the
	// model is missing them: an automatic teardown save must not overwrite the
	// source with what is left
	bool				m_Partial;
	bool			ApplyOne		(const SNqValue& op, xr_string& err);
	// drops selected ids that no longer exist (undo, remove_node, reload)
	void			PruneSelection	();
};

namespace NqDocs
{
	// opens (loads) or returns the already open document; `path` may be
	// project-relative. A missing file is an error (use quest_new / quest_write).
	ECORE_API NqDoc*	Open		(LPCSTR path, xr_string& err);
	// registers an empty document for a path that does not exist yet
	ECORE_API NqDoc*	Create		(LPCSTR path, xr_string& err);
	ECORE_API NqDoc*	Get			(LPCSTR path);
	// still registered? a window holding a document closed behind its back
	// (MCP quest_close) must drop it instead of drawing freed memory
	ECORE_API bool		IsOpen		(const NqDoc* d);
	// dirty documents refuse to close unless `discard` (err = "unsaved")
	ECORE_API bool		Close		(LPCSTR path, bool discard, xr_string& err);
	ECORE_API void		All			(xr_vector<NqDoc*>& out);
	// writes every dirty document to disk and logs one line per file (saved or
	// failed); returns the number saved. No dialogs - it runs on paths where a
	// modal would stall MCP and -nodlg (project switch, shutdown).
	ECORE_API int		SaveDirty	();
	// SaveDirty() first: no caller may drop unsaved work silently
	ECORE_API void		CloseAll	();
	// resolves to an absolute path (project-relative allowed) and appends
	// ".nqasset" when there is no extension; `for_write` additionally requires
	// the path to be inside the project (writes never leave the project folder)
	ECORE_API bool		Resolve		(LPCSTR path, xr_string& abs, xr_string& err, bool for_write = false);
	// true when `abs` lies inside the active project's folder
	ECORE_API bool		InsideProject(LPCSTR abs);

	// quest of an open document, else read from disk (quest_get on closed files);
	// `disk_only` ignores open documents (the build gate validates what ships)
	ECORE_API bool		ReadQuest	(LPCSTR path, SNqQuest& q, xr_string& err, bool disk_only = false);
	// validation context for a quest at `path` (other project quest ids, level)
	ECORE_API void		Validate	(const SNqQuest& q, LPCSTR path, xr_vector<SNqProblem>& out, bool disk_only = false);

	// every *.nqasset of the project (editor-only and source-only entries skipped)
	struct SProjectQuest
	{
		xr_string	path;		// absolute
		xr_string	id;			// "" when unreadable
		xr_string	title;
		bool		readable;
		xr_string	error;		// parse error when !readable
	};
	ECORE_API void		ProjectQuests(xr_vector<SProjectQuest>& out, bool disk_only = false);
	// ids of the project's quests except the one at `except_path`
	ECORE_API void		OtherQuestIds(LPCSTR except_path, xr_vector<xr_string>& out, bool disk_only = false);
	// name of the level of the current scene when it is a level of the linked game (E031)
	ECORE_API xr_string	CurrentLevel();
	// forget cached project scans (after writes from outside the doc layer)
	ECORE_API void		InvalidateProjectIndex();
}
