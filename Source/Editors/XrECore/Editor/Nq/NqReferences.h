#pragma once

#include "NqProjectIndex.h"

// Catalog-typed reference discovery. Parameter names are never guessed: a
// value is a reference only when the active catalog declares its exact type.
namespace NqReferences
{
	struct ECORE_API SReference
	{
		xr_string	path;
		xr_string	node;
		xr_string	slot;
		xr_string	kind;
		xr_string	param;
		xr_string	type;
		xr_string	value;
		xr_string	source;
	};

	struct ECORE_API SDiagnostic
	{
		xr_string	path;
		xr_string	node;
		xr_string	slot;
		xr_string	kind;
		xr_string	param;
		xr_string	code;
		xr_string	message;
		xr_string	source;

		xr_string	Text() const;
	};

	struct ECORE_API SResult
	{
		xr_vector<SReference>	references;
		xr_vector<SDiagnostic>	diagnostics;
		bool					complete;
		u32					generation;
		u64					fingerprint;
		u32					catalog_generation;

		SResult() : complete(true), generation(0), fingerprint(0), catalog_generation(0) {}
	};

	// `scope_path` is an absolute path key, or empty for the whole project.
	ECORE_API bool Find(const NqProjectIndex::SSnapshot& snapshot, LPCSTR type, LPCSTR value,
		LPCSTR scope_path, SResult& out, xr_string& err);

	// All diagnostics are produced before any value changes. Success changes the
	// declaration and every proven task_id reference in memory.
	ECORE_API bool RenameTask(SNqQuest& quest, LPCSTR path, LPCSTR from, LPCSTR to,
		int& references_updated, xr_vector<SDiagnostic>& diagnostics);
}
