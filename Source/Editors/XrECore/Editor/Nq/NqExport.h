#pragma once

// NQ - build gate and templates (docs/NQ_ARCHITECTURE.md par. 13.10). The module
// export copies .nqasset files as they are; what it adds is a refusal when
// any quest of the project has validation errors.

namespace NqExport
{
	// validates every *.nqasset of the project (parse, rules, unique ids).
	// False + err ("quest graph errors: N (see log)") when errors exist; every
	// problem goes to `log` (one line each) and to the editor log.
	ECORE_API bool	ValidateProject	(LPCSTR project_root, xr_string& err, xr_vector<xr_string>& log,
									 int* errors_out = 0, int* warnings_out = 0);
	// writes the minimal quest (trigger.start -> flow.end) at `path`; the id is
	// the file name, the title is the Russian "New quest". Refuses to overwrite.
	ECORE_API bool	NewQuestTemplate(LPCSTR path, xr_string& err);
	// canonical text of the template for `id`
	ECORE_API xr_string TemplateText(LPCSTR id);
}
