#include "stdafx.h"
#include "NqExport.h"
#include "NqDoc.h"
#include "NqLua.h"
#include "NqUtil.h"
#include "NqValidate.h"
#include "../EditorProject.h"

bool NqExport::ValidateProject(LPCSTR project_root, xr_string& err, xr_vector<xr_string>& log,
							   int* errors_out, int* warnings_out)
{
	err.clear();
	log.clear();
	if (errors_out) *errors_out = 0;
	if (warnings_out) *warnings_out = 0;
	if (!project_root || !project_root[0]) return true;

	// what ships is what is on disk: open documents with unsaved edits are
	// noted, not validated in their place
	xr_vector<NqDocs::SProjectQuest> quests;
	NqDocs::InvalidateProjectIndex();
	NqDocs::ProjectQuests(quests, true);
	if (quests.empty()) return true;

	int errors = 0, warnings = 0;
	for (u32 i = 0; i < quests.size(); ++i)
	{
		const xr_string rel = NqUtil::ProjectRelative(quests[i].path.c_str());
		if (NqDoc* d = NqDocs::Get(quests[i].path.c_str()))
			if (d->dirty) log.push_back(NqUtil::Format("%s: ~ has unsaved changes in the editor - the file on disk is what ships", rel.c_str()));
		SNqQuest q;
		xr_string rerr;
		if (!NqDocs::ReadQuest(quests[i].path.c_str(), q, rerr, true))
		{
			++errors;
			log.push_back(NqUtil::Format("%s: E003 %s", rel.c_str(), rerr.c_str()));
			continue;
		}
		xr_vector<SNqProblem> problems;
		NqDocs::Validate(q, quests[i].path.c_str(), problems, true);
		for (u32 p = 0; p < problems.size(); ++p)
		{
			if (problems[p].IsError()) ++errors; else ++warnings;
			log.push_back(rel + ": " + problems[p].Text());
		}
	}
	for (u32 i = 0; i < log.size(); ++i) Msg("%s [nq] %s", (log[i].find(": E") != xr_string::npos) ? "!" : "~", log[i].c_str());
	if (errors_out) *errors_out = errors;
	if (warnings_out) *warnings_out = warnings;
	if (errors)
	{
		err = NqUtil::Format("quest graph errors: %d (see log)", errors);
		return false;
	}
	return true;
}

xr_string NqExport::TemplateText(LPCSTR id)
{
	SNqQuest q;
	q.nq = 1;
	q.id = (id && id[0]) ? id : "new_quest";
	q.title = SNqValue::String("\xD0\x9D\xD0\xBE\xD0\xB2\xD1\x8B\xD0\xB9 \xD0\xBA\xD0\xB2\xD0\xB5\xD1\x81\xD1\x82");	// UTF-8 for the Russian "New quest"
	q.activation = "auto";
	SNqNode start;
	start.id = "start"; start.kind = "trigger.start";
	start.Connect("next", "finish");
	SNqNode finish;
	finish.id = "finish"; finish.kind = "flow.end";
	finish.params = SNqValue::Table();
	finish.params.Set("status", SNqValue::String("completed"));
	q.nodes.push_back(start);
	q.nodes.push_back(finish);
	return NqLua::Write(q);
}

bool NqExport::NewQuestTemplate(LPCSTR path, xr_string& err)
{
	err.clear();
	if (!path || !path[0]) { err = "path is empty"; return false; }
	if (NqUtil::FileExists(path)) { err = "file already exists"; return false; }
	xr_string id = NqUtil::BaseName(path);
	// the file name is the id: keep it valid ([a-z0-9_]) so W061 stays quiet
	for (u32 i = 0; i < id.size(); ++i)
	{
		char c = (char)tolower((unsigned char)id[i]);
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
		id[i] = c;
	}
	if (id.empty()) id = "new_quest";
	if (!NqLua::WriteFile(path, TemplateText(id.c_str()), err)) return false;
	NqDocs::InvalidateProjectIndex();
	return true;
}
