#pragma once

#include "NqAsset.h"

// A read-only project view for tools that must inspect every quest without
// opening editor documents. Dirty documents overlay their disk entry, while
// clean documents deliberately do not hide external file changes.
class ECORE_API NqProjectIndex
{
public:
	struct SEntry
	{
		xr_string	path;
		xr_string	relative;
		SNqQuest	quest;
		xr_string	error;
		bool		readable;
		bool		complete;
		bool		memory;
		u64			content_hash;

		SEntry() : readable(false), complete(false), memory(false), content_hash(0) {}
		LPCSTR Source() const { return memory ? "memory" : "disk"; }
	};

	struct SSnapshot
	{
		xr_vector<SEntry>	entries;
		u32					generation;
		u64					fingerprint;

		SSnapshot() : generation(0), fingerprint(0) {}
	};

	struct SDiagnostic
	{
		xr_string	path;
		xr_string	code;
		xr_string	message;
		xr_string	source;
	};

	struct SFindResult
	{
		xr_string	path;
		xr_string	node;
		xr_string	kind;
		xr_string	title;
		xr_string	match;
		xr_string	source;
	};

	struct SFindResponse
	{
		xr_vector<SFindResult>	results;
		xr_vector<SDiagnostic>	diagnostics;
		bool					complete;
		u32					generation;
		u64					fingerprint;

		SFindResponse() : complete(true), generation(0), fingerprint(0) {}
	};

	// Directly walks a sorted *.nqasset list. Parsing is reused only when the
	// exact file-content hash is unchanged.
	static bool	Snapshot		(SSnapshot& out, xr_string& err, bool overlay_dirty = true);
	static void	Invalidate		();
	static u32	InvalidationSerial();
	static void	FindNodes		(const SSnapshot& snapshot, LPCSTR query, SFindResponse& out);
	static xr_string FingerprintText(u64 fingerprint);
};
