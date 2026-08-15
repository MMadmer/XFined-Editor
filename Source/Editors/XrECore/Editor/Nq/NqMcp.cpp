#include "stdafx.h"
#include "NqMcp.h"
#include "NqDoc.h"
#include "NqLua.h"
#include "NqUtil.h"
#include "NqCatalog.h"
#include "NqPickers.h"
#include "NqExport.h"
#include "NqValidate.h"
#include "../EditorProject.h"

namespace
{
	// {"ok":true,"path":...,"id":...,"errors":N,"warnings":N,"problems":[...],"outline":...}
	void AppendSummary(xr_string& out, LPCSTR path, const SNqQuest& q, const xr_vector<SNqProblem>& problems,
					   bool with_outline, bool with_lua)
	{
		out += "\"path\":";			NqUtil::JsonPath(out, NqUtil::ProjectRelative(path).c_str());
		out += ",\"id\":";			NqUtil::JsonString(out, q.id);
		out += ",\"title\":";		NqUtil::JsonString(out, NqText::Preview(q.title));
		out += ",\"nodes\":";		NqUtil::JsonInt(out, (int)q.nodes.size());
		out += ",\"errors\":";		NqUtil::JsonInt(out, NqValidate::CountErrors(problems));
		out += ",\"warnings\":";	NqUtil::JsonInt(out, NqValidate::CountWarnings(problems));
		out += ",\"problems\":";	NqValidate::ToJson(problems, out);
		if (with_outline)
		{
			xr_string ol = NqLua::Outline(q);
			ol += NqValidate::ToText(problems);
			out += ",\"outline\":";	NqUtil::JsonString(out, ol);
		}
		if (with_lua)
		{
			out += ",\"lua\":";		NqUtil::JsonString(out, NqLua::Write(q));
		}
	}

	void AppendDocState(xr_string& out, const NqDoc* d)
	{
		out += ",\"open\":";	NqUtil::JsonBool(out, d != 0);
		out += ",\"dirty\":";	NqUtil::JsonBool(out, d && d->dirty);
		if (d)
		{
			out += ",\"undo\":";	NqUtil::JsonInt(out, d->UndoDepth());
			out += ",\"redo\":";	NqUtil::JsonInt(out, d->RedoDepth());
		}
	}

	bool NeedPath(LPCSTR raw, xr_string& path, xr_string& out)
	{
		NqUtil::ArgString(raw, "path", path);
		if (path.empty()) { NqUtil::JsonError(out, "missing 'path' (project-relative, e.g. quests/wolf_debt.nqasset)"); return false; }
		return true;
	}

	// a document that exists on disk opens on demand; MCP works without a canvas
	NqDoc* OpenForEdit(const xr_string& path, xr_string& out)
	{
		xr_string err;
		NqDoc* d = NqDocs::Open(path.c_str(), err);
		if (!d) NqUtil::JsonError(out, err);
		return d;
	}

	// Lua table text may come with or without `return`
	xr_string AsChunk(const xr_string& text)
	{
		xr_string t = NqUtil::Trim(text);
		if (!t.empty() && t[0] == '{') return "return " + t;
		return text;
	}

	void RespondDoc(xr_string& out, NqDoc* d, bool with_outline, bool with_lua)
	{
		out = "{\"ok\":true,";
		AppendSummary(out, d->path.c_str(), d->quest, d->problems, with_outline, with_lua);
		AppendDocState(out, d);
		out += "}";
	}

	// ---- commands ------------------------------------------------------------------
	void CmdList(xr_string& out)
	{
		if (!EditorProject::Active()) { NqUtil::JsonError(out, "no active project"); return; }
		xr_vector<NqDocs::SProjectQuest> all;
		NqDocs::InvalidateProjectIndex();
		NqDocs::ProjectQuests(all);
		out = "{\"ok\":true,\"count\":";
		NqUtil::JsonInt(out, (int)all.size());
		out += ",\"quests\":[";
		for (u32 i = 0; i < all.size(); ++i)
		{
			if (i) out += ",";
			out += "{\"path\":"; NqUtil::JsonPath(out, NqUtil::ProjectRelative(all[i].path.c_str()).c_str());
			if (!all[i].readable)
			{
				out += ",\"readable\":false,\"error\":"; NqUtil::JsonString(out, all[i].error);
				out += ",\"errors\":1,\"warnings\":0}";
				continue;
			}
			SNqQuest q;
			xr_string err;
			xr_vector<SNqProblem> problems;
			NqDoc* d = NqDocs::Get(all[i].path.c_str());
			if (d) { q = d->quest; problems = d->problems; }
			else if (NqDocs::ReadQuest(all[i].path.c_str(), q, err)) NqDocs::Validate(q, all[i].path.c_str(), problems);
			out += ",\"readable\":true,\"id\":";	NqUtil::JsonString(out, q.id);
			out += ",\"title\":";					NqUtil::JsonString(out, NqText::Preview(q.title));
			out += ",\"nodes\":";					NqUtil::JsonInt(out, (int)q.nodes.size());
			out += ",\"errors\":";					NqUtil::JsonInt(out, NqValidate::CountErrors(problems));
			out += ",\"warnings\":";				NqUtil::JsonInt(out, NqValidate::CountWarnings(problems));
			out += ",\"open\":";					NqUtil::JsonBool(out, d != 0);
			out += ",\"dirty\":";					NqUtil::JsonBool(out, d && d->dirty);
			out += "}";
		}
		out += "]}";
	}

	void CmdNew(LPCSTR raw, xr_string& out)
	{
		xr_string path;
		if (!NeedPath(raw, path, out)) return;
		xr_string abs, err;
		if (!NqDocs::Resolve(path.c_str(), abs, err, true)) { NqUtil::JsonError(out, err); return; }
		if (!NqExport::NewQuestTemplate(abs.c_str(), err)) { NqUtil::JsonError(out, err); return; }
		NqDoc* d = NqDocs::Open(abs.c_str(), err);
		if (!d) { NqUtil::JsonError(out, err); return; }
		RespondDoc(out, d, true, true);
	}

	void CmdOpen(LPCSTR raw, xr_string& out)
	{
		xr_string path;
		if (!NeedPath(raw, path, out)) return;
		NqDoc* d = OpenForEdit(path, out);
		if (!d) return;
		RespondDoc(out, d, true, false);
	}

	void CmdClose(LPCSTR raw, xr_string& out)
	{
		xr_string path;
		if (!NeedPath(raw, path, out)) return;
		xr_string err;
		if (!NqDocs::Close(path.c_str(), NqUtil::ArgBool(raw, "discard", false), err)) { NqUtil::JsonError(out, err); return; }
		out = "{\"ok\":true}";
	}

	void CmdGet(LPCSTR raw, xr_string& out)
	{
		xr_string path;
		if (!NeedPath(raw, path, out)) return;
		if (NqDoc* d = NqDocs::Get(path.c_str())) { RespondDoc(out, d, true, true); return; }
		xr_string abs, err;
		if (!NqDocs::Resolve(path.c_str(), abs, err)) { NqUtil::JsonError(out, err); return; }
		SNqQuest q;
		if (!NqDocs::ReadQuest(abs.c_str(), q, err)) { NqUtil::JsonError(out, err); return; }
		xr_vector<SNqProblem> problems;
		NqDocs::Validate(q, abs.c_str(), problems);
		out = "{\"ok\":true,";
		AppendSummary(out, abs.c_str(), q, problems, true, true);
		AppendDocState(out, 0);
		out += "}";
	}

	void CmdWrite(LPCSTR raw, xr_string& out)
	{
		xr_string path, lua;
		if (!NeedPath(raw, path, out)) return;
		if (!NqUtil::ArgString(raw, "lua", lua) || lua.empty()) { NqUtil::JsonError(out, "missing 'lua' (the whole file text: return { nq = 1, ... })"); return; }
		xr_string abs, err;
		if (!NqDocs::Resolve(path.c_str(), abs, err, true)) { NqUtil::JsonError(out, err); return; }
		const xr_string chunk = AsChunk(lua);
		if (NqDoc* d = NqDocs::Get(abs.c_str()))
		{
			// open document: replaced in memory (undo), saved by quest_save
			if (!d->SetFromLua(chunk, err)) { NqUtil::JsonError(out, err); return; }
			out = "{\"ok\":true,\"saved\":false,";
			AppendSummary(out, d->path.c_str(), d->quest, d->problems, true, false);
			AppendDocState(out, d);
			out += "}";
			return;
		}
		// closed (or new) file: canonical text goes to disk when it parses;
		// validation problems are reported, only the build refuses them
		SNqValue v;
		NqLua::SError e;
		if (!NqLua::ParseValue(chunk.c_str(), (u32)chunk.size(), NqUtil::BaseName(abs.c_str()).c_str(), v, e))
		{
			xr_string msg = e.message;
			if (e.line > 0 && e.col > 0)	msg += NqUtil::Format(" (line %d:%d)", e.line, e.col);
			else if (e.line > 0)			msg += NqUtil::Format(" (line %d)", e.line);
			NqUtil::JsonError(out, msg);
			return;
		}
		SNqQuest q;
		xr_vector<xr_string> shape;
		NqLua::QuestFromValue(v, q, shape);
		// what the reader could not type is DROPPED, and what goes to disk is the
		// canonical text of the model - writing it would replace the caller's file
		// with a quest missing exactly the parts the message complains about
		if (!shape.empty())
		{
			xr_string msg = "nothing written - the editor cannot represent: " + shape[0];
			for (u32 i = 1; i < shape.size(); ++i) { msg += "; "; msg += shape[i]; }
			NqUtil::JsonError(out, msg);
			return;
		}
		if (!NqLua::WriteFile(abs.c_str(), NqLua::Write(q), err)) { NqUtil::JsonError(out, err + ": " + NqUtil::ProjectRelative(abs.c_str())); return; }
		NqDocs::InvalidateProjectIndex();
		xr_vector<SNqProblem> problems;
		NqValidate::ShapeErrors(shape, problems);
		NqDocs::Validate(q, abs.c_str(), problems);
		out = "{\"ok\":true,\"saved\":true,";
		AppendSummary(out, abs.c_str(), q, problems, true, false);
		AppendDocState(out, 0);
		out += "}";
	}

	void CmdUndoRedo(LPCSTR raw, bool undo, xr_string& out)
	{
		xr_string path;
		if (!NeedPath(raw, path, out)) return;
		NqDoc* d = NqDocs::Get(path.c_str());
		if (!d) { NqUtil::JsonError(out, "not open"); return; }
		const bool done = undo ? d->Undo() : d->Redo();
		if (!done) { NqUtil::JsonError(out, undo ? "nothing to undo" : "nothing to redo"); return; }
		RespondDoc(out, d, true, false);
	}

	void CmdApply(LPCSTR raw, xr_string& out)
	{
		xr_string path, ops;
		if (!NeedPath(raw, path, out)) return;
		if (!NqUtil::ArgString(raw, "ops", ops) || ops.empty()) { NqUtil::JsonError(out, "missing 'ops' (Lua list of operations: { { op = \"add_node\", node = { ... } }, ... })"); return; }
		NqDoc* d = OpenForEdit(path, out);
		if (!d) return;
		SNqValue v;
		NqLua::SError e;
		const xr_string chunk = AsChunk(ops);
		if (!NqLua::ParseValue(chunk.c_str(), (u32)chunk.size(), "ops", v, e))
		{
			xr_string msg = "ops: " + e.message;
			if (e.line > 0) msg += NqUtil::Format(" (line %d)", e.line);
			NqUtil::JsonError(out, msg);
			return;
		}
		xr_string err;
		int applied = 0;
		if (!d->ApplyOps(v, err, applied)) { NqUtil::JsonError(out, err); return; }
		out = "{\"ok\":true,\"applied\":";
		NqUtil::JsonInt(out, applied);
		out += ",";
		AppendSummary(out, d->path.c_str(), d->quest, d->problems, true, false);
		AppendDocState(out, d);
		out += "}";
	}

	void CmdValidate(LPCSTR raw, xr_string& out)
	{
		xr_string path;
		if (!NeedPath(raw, path, out)) return;
		if (NqDoc* d = NqDocs::Get(path.c_str())) { d->Validate(); RespondDoc(out, d, true, false); return; }
		xr_string abs, err;
		if (!NqDocs::Resolve(path.c_str(), abs, err)) { NqUtil::JsonError(out, err); return; }
		SNqQuest q;
		if (!NqDocs::ReadQuest(abs.c_str(), q, err)) { NqUtil::JsonError(out, err); return; }
		xr_vector<SNqProblem> problems;
		NqDocs::Validate(q, abs.c_str(), problems);
		out = "{\"ok\":true,";
		AppendSummary(out, abs.c_str(), q, problems, true, false);
		AppendDocState(out, 0);
		out += "}";
	}

	void CmdSave(LPCSTR raw, xr_string& out)
	{
		xr_string path;
		if (!NeedPath(raw, path, out)) return;
		NqDoc* d = NqDocs::Get(path.c_str());
		if (!d) { NqUtil::JsonError(out, "not open"); return; }
		xr_string err;
		if (!d->Save(NqUtil::ArgBool(raw, "force", false), err)) { NqUtil::JsonError(out, err); return; }
		out = "{\"ok\":true,\"path\":";
		NqUtil::JsonPath(out, NqUtil::ProjectRelative(d->path.c_str()).c_str());
		out += ",\"errors\":";	NqUtil::JsonInt(out, d->ErrorCount());
		out += ",\"warnings\":"; NqUtil::JsonInt(out, d->WarningCount());
		out += "}";
	}

	void CmdReload(LPCSTR raw, xr_string& out)
	{
		xr_string path;
		if (!NeedPath(raw, path, out)) return;
		NqDoc* d = NqDocs::Get(path.c_str());
		if (!d) { NqUtil::JsonError(out, "not open"); return; }
		xr_string err;
		if (!d->Reload(err)) { NqUtil::JsonError(out, err); return; }
		RespondDoc(out, d, true, false);
	}

	void CmdLayout(LPCSTR raw, xr_string& out)
	{
		xr_string path;
		if (!NeedPath(raw, path, out)) return;
		NqDoc* d = OpenForEdit(path, out);
		if (!d) return;
		const int moved = d->Layout(!NqUtil::ArgBool(raw, "all", true));
		out = "{\"ok\":true,\"moved\":";
		NqUtil::JsonInt(out, moved);
		out += ",\"positions\":[";
		for (u32 i = 0; i < d->quest.nodes.size(); ++i)
		{
			const SNqNode& n = d->quest.nodes[i];
			if (i) out += ",";
			out += "{\"id\":"; NqUtil::JsonString(out, n.id);
			out += ",\"x\":"; NqUtil::JsonNumber(out, n.pos.x);
			out += ",\"y\":"; NqUtil::JsonNumber(out, n.pos.y);
			out += "}";
		}
		out += "]";
		AppendDocState(out, d);
		out += "}";
	}

	void CmdCheckAll(xr_string& out)
	{
		if (!EditorProject::Active()) { NqUtil::JsonError(out, "no active project"); return; }
		xr_string err;
		xr_vector<xr_string> log;
		int errors = 0, warnings = 0;
		const bool passed = NqExport::ValidateProject(EditorProject::Root(), err, log, &errors, &warnings);
		out = "{\"ok\":true,\"passed\":";
		NqUtil::JsonBool(out, passed);
		out += ",\"errors\":";		NqUtil::JsonInt(out, errors);
		out += ",\"warnings\":";	NqUtil::JsonInt(out, warnings);
		out += ",\"error\":";		NqUtil::JsonString(out, err);
		out += ",\"log\":";			NqUtil::JsonStringArray(out, log);
		out += "}";
	}
}

bool NqMcp::Handle(LPCSTR cmd, LPCSTR raw, xr_string& out)
{
	if (!cmd || 0 != strncmp(cmd, "quest_", 6)) return false;
	if (0 == strcmp(cmd, "quest_catalog"))		{ NqCatalog::McpCatalog(out);		return true; }
	if (0 == strcmp(cmd, "quest_list"))			{ CmdList(out);						return true; }
	if (0 == strcmp(cmd, "quest_new"))			{ CmdNew(raw, out);					return true; }
	if (0 == strcmp(cmd, "quest_open"))			{ CmdOpen(raw, out);				return true; }
	if (0 == strcmp(cmd, "quest_close"))		{ CmdClose(raw, out);				return true; }
	if (0 == strcmp(cmd, "quest_get"))			{ CmdGet(raw, out);					return true; }
	if (0 == strcmp(cmd, "quest_write"))		{ CmdWrite(raw, out);				return true; }
	if (0 == strcmp(cmd, "quest_undo"))			{ CmdUndoRedo(raw, true, out);		return true; }
	if (0 == strcmp(cmd, "quest_redo"))			{ CmdUndoRedo(raw, false, out);		return true; }
	if (0 == strcmp(cmd, "quest_apply"))		{ CmdApply(raw, out);				return true; }
	if (0 == strcmp(cmd, "quest_validate"))		{ CmdValidate(raw, out);			return true; }
	if (0 == strcmp(cmd, "quest_save"))			{ CmdSave(raw, out);				return true; }
	if (0 == strcmp(cmd, "quest_reload"))		{ CmdReload(raw, out);				return true; }
	if (0 == strcmp(cmd, "quest_layout"))		{ CmdLayout(raw, out);				return true; }
	if (0 == strcmp(cmd, "quest_lookup"))		{ NqPickers::McpLookup(raw, out);	return true; }
	if (0 == strcmp(cmd, "quest_check_all"))	{ CmdCheckAll(out);					return true; }
	// quest_view is Phase 4 (UI); anything else quest_* is unknown here
	return false;
}
