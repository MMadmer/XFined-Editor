#pragma once

// NQ - deterministic top-down auto layout (docs/NQ_ARCHITECTURE.md par. 13.8):
// a pin-aware dominator tree keeps complete branch bounds apart and promotes
// shared descendants into merge blocks between their contributing branches.
// Exclusive trigger trees and root-level merge blocks use a larger gap.

#include "NqAsset.h"

namespace NqLayout
{
	// canvas metrics shared with the Phase 4 canvas (world units at zoom 1.0)
	const float kNodeWidth		= 240.f;
	const float kNodeHeight		= 96.f;		// body without chip strips
	const float kChipStrip		= 26.f;		// one on_enter / on_exit strip
	const float kTriggerHeight	= 56.f;
	const float kGapX			= 64.f;
	const float kTreeGapX		= 160.f;
	const float kGapY			= 72.f;
	const float kGrid			= 16.f;
	// world coordinates are bounded: a hand-written or corrupt .nqasset can carry
	// inf / NaN / 1e18, which neither this layout nor the canvas renderer survives
	const float kWorldMax		= 1.0e6f;

	// finite and inside +-kWorldMax (non-finite becomes 0)
	ECORE_API float		Sane		(float v);
	// clamps every node position; returns the number of nodes it had to fix
	ECORE_API int		SanePositions(SNqQuest& q);
	// clamps positions and runs the graph layout only when a node has no position
	ECORE_API int		EnsurePositions(SNqQuest& q);

	// estimated size of a node box
	ECORE_API Fvector2	NodeSize	(const SNqNode& n);
	// positions every node (`only_missing` = keep nodes that already have pos
	// and place the rest below the existing layout); returns the number of
	// nodes moved
	ECORE_API int		Run			(SNqQuest& q, bool only_missing);
}
