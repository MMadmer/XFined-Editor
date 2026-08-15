#pragma once

// NQ - the validator (docs/NQ_ARCHITECTURE.md par. 15). One rule list with the
// same codes the game runtime prints; errors block the build, warnings do not.

#include "NqAsset.h"

struct ECORE_API SNqProblem
{
	enum ESeverity { sevError = 0, sevWarning = 1 };

	ESeverity	severity;
	xr_string	code;			// "E007", "W011", ...
	xr_string	node_id;		// empty = quest level
	xr_string	slot;			// "enter:2" / "exit:0" / "cond:1" / "param:npc" / "out:next" / "case:1" / ""
	xr_string	message;		// UTF-8

	SNqProblem() : severity(sevError) {}
	bool IsError() const { return severity == sevError; }
	// "E007 node[slot]: message"
	xr_string	Text() const;
};

namespace NqValidate
{
	struct SContext
	{
		LPCSTR						file_path;			// for W061 (0 = skip)
		const xr_vector<xr_string>*	project_quest_ids;	// ids of the OTHER quests of the project (E002) and
														// all bare quest_id references (E030); 0 = skip
		LPCSTR						level;				// current scene level for E031 (0 = skip)
		bool						use_pickers;		// W060 through NqPickers (needs a linked game)
		SContext() : file_path(0), project_quest_ids(0), level(0), use_pickers(true) {}
	};

	// full rule set; appends to `out`
	ECORE_API void	Run				(const SNqQuest& q, const SContext& ctx, xr_vector<SNqProblem>& out);
	// reader shape errors -> E003 problems
	ECORE_API void	ShapeErrors		(const xr_vector<xr_string>& shape, xr_vector<SNqProblem>& out);
	ECORE_API int	CountErrors		(const xr_vector<SNqProblem>& v);
	ECORE_API int	CountWarnings	(const xr_vector<SNqProblem>& v);
	// JSON array of {code,severity,node,slot,message}
	ECORE_API void	ToJson			(const xr_vector<SNqProblem>& v, xr_string& out);
	// "problems: none" or one line per problem
	ECORE_API xr_string	ToText		(const xr_vector<SNqProblem>& v);

	// ref names declared by the quest (spawn.*.ref, item.spawn.ref, objective.kill spawn ref)
	ECORE_API void	DeclaredRefs	(const SNqQuest& q, xr_vector<xr_string>& out);
}
