#include "stdafx.h"
#include "NqLayout.h"

Fvector2 NqLayout::NodeSize(const SNqNode& n)
{
	Fvector2 s;
	if (NqText::IsTrigger(n.kind.c_str())) { s.set(kNodeWidth, kTriggerHeight); return s; }
	// both chip strips are always drawn (an empty one still carries its "+"),
	// so both are always part of the box - otherwise the coloured body of a
	// node without actions is squeezed and its title runs into the summary
	s.set(kNodeWidth, kNodeHeight + 2.f * kChipStrip);
	return s;
}

namespace
{
	struct SLayoutNode
	{
		int		layer;
		float	bary;
		int		file_order;
		float	x, y, w, h;
	};

	float Snap(float v) { return floorf(v / NqLayout::kGrid + 0.5f) * NqLayout::kGrid; }
}

float NqLayout::Sane(float v)
{
	if (!_finite(v) || _isnan(v)) return 0.f;
	if (v < -kWorldMax) return -kWorldMax;
	if (v > kWorldMax) return kWorldMax;
	return v;
}

int NqLayout::SanePositions(SNqQuest& q)
{
	int fixed = 0;
	for (u32 i = 0; i < q.nodes.size(); ++i)
	{
		Fvector2& p = q.nodes[i].pos;
		const float x = Sane(p.x), y = Sane(p.y);
		// NaN compares unequal to itself, so this also catches it
		if (!(x == p.x) || !(y == p.y)) { p.set(x, y); ++fixed; }
	}
	return fixed;
}

int NqLayout::Run(SNqQuest& q, bool only_missing)
{
	const u32 count = (u32)q.nodes.size();
	if (!count) return 0;
	// every path that changes a quest runs through here (load, quest_apply,
	// paste, drag), so this is the one place a bad coordinate can be caught
	SanePositions(q);
	xr_vector<SLayoutNode> L(count);
	for (u32 i = 0; i < count; ++i)
	{
		L[i].layer = -1; L[i].bary = 0.f; L[i].file_order = (int)i;
		const Fvector2 sz = NodeSize(q.nodes[i]);
		L[i].w = sz.x; L[i].h = sz.y; L[i].x = 0.f; L[i].y = 0.f;
	}

	// ---- layers: BFS from the triggers, then from whatever is still unplaced ----
	xr_vector<int> queue;
	int max_layer = -1;
	auto bfs = [&](int start, int start_layer)
	{
		queue.clear();
		if (L[start].layer >= 0) return;
		L[start].layer = start_layer;
		queue.push_back(start);
		for (u32 qi = 0; qi < queue.size(); ++qi)
		{
			const int cur = queue[qi];
			max_layer = std::max(max_layer, L[cur].layer);
			const SNqNode& n = q.nodes[cur];
			for (u32 p = 0; p < n.out.size(); ++p)
				for (u32 t = 0; t < n.out[p].second.size(); ++t)
				{
					const int j = q.NodeIndex(n.out[p].second[t].c_str());
					if (j < 0 || L[j].layer >= 0) continue;
					L[j].layer = L[cur].layer + 1;
					queue.push_back(j);
				}
		}
	};
	// all triggers share layer 0 and are expanded together (true BFS = shortest depth)
	{
		queue.clear();
		for (u32 i = 0; i < count; ++i)
			if (NqText::IsTrigger(q.nodes[i].kind.c_str())) { L[i].layer = 0; queue.push_back((int)i); max_layer = 0; }
		for (u32 qi = 0; qi < queue.size(); ++qi)
		{
			const int cur = queue[qi];
			const SNqNode& n = q.nodes[cur];
			for (u32 p = 0; p < n.out.size(); ++p)
				for (u32 t = 0; t < n.out[p].second.size(); ++t)
				{
					const int j = q.NodeIndex(n.out[p].second[t].c_str());
					if (j < 0 || L[j].layer >= 0) continue;
					L[j].layer = L[cur].layer + 1;
					max_layer = std::max(max_layer, L[j].layer);
					queue.push_back(j);
				}
		}
	}
	for (u32 i = 0; i < count; ++i)
		if (L[i].layer < 0) bfs((int)i, max_layer + 1);

	// ---- per layer: order by barycenter of parents, then place ------------------
	const int layers = max_layer + 1;
	xr_vector<xr_vector<int> > by_layer(layers);
	for (u32 i = 0; i < count; ++i) by_layer[L[i].layer].push_back((int)i);

	float y = 0.f;
	for (int layer = 0; layer < layers; ++layer)
	{
		xr_vector<int>& members = by_layer[layer];
		// barycenter = mean center x of the parents already placed (earlier layers)
		for (u32 m = 0; m < members.size(); ++m)
		{
			const int i = members[m];
			float sum = 0.f; int n = 0;
			for (u32 s = 0; s < count; ++s)
			{
				if (L[s].layer >= layer) continue;
				if (q.nodes[s].HasTarget(q.nodes[i].id.c_str())) { sum += L[s].x + L[s].w * 0.5f; ++n; }
			}
			L[i].bary = n ? sum / n : 0.f;
		}
		std::stable_sort(members.begin(), members.end(), [&](int a, int b)
		{
			if (L[a].bary != L[b].bary) return L[a].bary < L[b].bary;
			return L[a].file_order < L[b].file_order;
		});
		float total = 0.f, max_h = 0.f, center = 0.f;
		for (u32 m = 0; m < members.size(); ++m)
		{
			total += L[members[m]].w;
			max_h = std::max(max_h, L[members[m]].h);
			center += L[members[m]].bary;
		}
		total += kGapX * (members.size() > 1 ? (float)(members.size() - 1) : 0.f);
		center = members.empty() ? 0.f : center / (float)members.size();
		float x = center - total * 0.5f;
		for (u32 m = 0; m < members.size(); ++m)
		{
			SLayoutNode& ln = L[members[m]];
			ln.x = Snap(x);
			ln.y = Snap(y);
			x += ln.w + kGapX;
		}
		y += max_h + kGapY;
	}

	// ---- apply --------------------------------------------------------------------
	int moved = 0;
	if (only_missing)
	{
		bool any_existing = false;
		float max_y = -1e9f, min_missing_y = 1e9f;
		for (u32 i = 0; i < count; ++i)
		{
			if (q.nodes[i].has_pos) { any_existing = true; max_y = std::max(max_y, q.nodes[i].pos.y + L[i].h); }
			else min_missing_y = std::min(min_missing_y, L[i].y);
		}
		const float dy = any_existing ? Snap(max_y + kGapY - min_missing_y) : 0.f;
		for (u32 i = 0; i < count; ++i)
		{
			if (q.nodes[i].has_pos) continue;
			q.nodes[i].pos.set(L[i].x, L[i].y + dy);
			q.nodes[i].has_pos = true;
			++moved;
		}
		return moved;
	}
	for (u32 i = 0; i < count; ++i)
	{
		if (!q.nodes[i].has_pos || q.nodes[i].pos.x != L[i].x || q.nodes[i].pos.y != L[i].y) ++moved;
		q.nodes[i].pos.set(L[i].x, L[i].y);
		q.nodes[i].has_pos = true;
	}
	return moved;
}
