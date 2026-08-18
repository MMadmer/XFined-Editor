#include "stdafx.h"
#include "NqLayout.h"
#include "NqCatalog.h"

Fvector2 NqLayout::NodeSize(const SNqNode& n)
{
	Fvector2 s;
	if (NqText::IsTrigger(n.kind.c_str())) { s.set(kNodeWidth, kTriggerHeight); return s; }
	// Both chip strips are always drawn because an empty strip still carries its add button.
	s.set(kNodeWidth, kNodeHeight + 2.f * kChipStrip);
	return s;
}

namespace
{
	struct SLayoutNode
	{
		int		layer;
		int		parent;
		int		root;
		float	side_sum;
		int		side_count;
		float	x, y, w, h;
		float	center_x;
		float	offset_x;
		float	min_x, max_x;
		xr_vector<int> children;
	};

	struct SBlock
	{
		float min_x, max_x;
		SBlock() : min_x(0.f), max_x(0.f) {}
	};

	float Snap(float v) { return floorf(v / NqLayout::kGrid + 0.5f) * NqLayout::kGrid; }

	void BuildPinOrder(const SNqNode& n, xr_vector<xr_string>& pins)
	{
		pins.clear();
		const NqCatalog::SKind* kind = NqCatalog::Find(n.kind.c_str());
		if (kind)
		{
			// This must mirror NqCanvas::BuildGeometry: layout sides are the visible pin sides.
			if (kind->pins_from_cases)
			{
				const SNqValue* cases = n.params.Get("cases");
				if (cases && cases->IsTable())
					for (u32 i = 0; i < cases->arr.size(); ++i)
					{
						xr_string name = cases->arr[i].GetString("name");
						if (!name.empty()) pins.push_back(name);
					}
			}
			for (u32 i = 0; i < kind->pins.size(); ++i)
				if (kind->pins[i] != "cases") pins.push_back(kind->pins[i]);
		}
		for (u32 i = 0; i < n.out.size(); ++i)
		{
			bool found = false;
			for (u32 p = 0; p < pins.size(); ++p)
				if (pins[p] == n.out[i].first) { found = true; break; }
			if (!found) pins.push_back(n.out[i].first);
		}
	}

	float PinSide(int index, int count)
	{
		if (count <= 1) return 0.f;
		return 2.f * float(index + 1) / float(count + 1) - 1.f;
	}

	SBlock PackBlock(xr_vector<SLayoutNode>& nodes, const xr_vector<int>& members, float gap)
	{
		SBlock block;
		float cursor = 0.f;
		for (u32 i = 0; i < members.size(); ++i)
		{
			SLayoutNode& child = nodes[members[i]];
			child.offset_x = cursor - child.min_x;
			cursor += child.max_x - child.min_x;
			if (i + 1 < members.size()) cursor += gap;
		}
		block.max_x = cursor;
		return block;
	}

	void ShiftBlock(xr_vector<SLayoutNode>& nodes, const xr_vector<int>& members, SBlock& block, float shift)
	{
		for (u32 i = 0; i < members.size(); ++i) nodes[members[i]].offset_x += shift;
		block.min_x += shift;
		block.max_x += shift;
	}

	void PackRoots(xr_vector<SLayoutNode>& nodes, const xr_vector<int>& roots)
	{
		if (roots.empty()) return;
		float cursor = 0.f;
		for (u32 i = 0; i < roots.size(); ++i)
		{
			SLayoutNode& root = nodes[roots[i]];
			root.center_x = cursor - root.min_x;
			cursor += root.max_x - root.min_x;
			if (i + 1 < roots.size()) cursor += NqLayout::kTreeGapX;
		}
		const float shift = -cursor * 0.5f;
		for (u32 i = 0; i < roots.size(); ++i) nodes[roots[i]].center_x += shift;
	}
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
		// NaN compares unequal to itself, so this also catches it.
		if (!(x == p.x) || !(y == p.y)) { p.set(x, y); ++fixed; }
	}
	return fixed;
}

int NqLayout::EnsurePositions(SNqQuest& q)
{
	SanePositions(q);
	for (const SNqNode& node : q.nodes)
		if (!node.has_pos) return Run(q, true);
	return 0;
}

int NqLayout::Run(SNqQuest& q, bool only_missing)
{
	const u32 count = (u32)q.nodes.size();
	if (!count) return 0;
	SanePositions(q);

	xr_vector<SLayoutNode> layout(count);
	for (u32 i = 0; i < count; ++i)
	{
		SLayoutNode& node = layout[i];
		node.layer = -1;
		node.parent = -1;
		node.root = -1;
		node.side_sum = 0.f;
		node.side_count = 0;
		node.x = node.y = node.center_x = node.offset_x = 0.f;
		const Fvector2 size = NodeSize(q.nodes[i]);
		node.w = size.x;
		node.h = size.y;
		node.min_x = -size.x * 0.5f;
		node.max_x = size.x * 0.5f;
	}

	xr_vector<int> roots;
	xr_vector<int> orphan_roots;
	xr_vector<int> queue;
	xr_vector<int> order;
	int max_layer = -1;
	for (u32 i = 0; i < count; ++i)
		if (NqText::IsTrigger(q.nodes[i].kind.c_str()))
		{
			layout[i].layer = 0;
			layout[i].root = (int)i;
			roots.push_back((int)i);
			queue.push_back((int)i);
			order.push_back((int)i);
			max_layer = 0;
		}

	auto expand = [&](xr_vector<int>& work)
	{
		xr_vector<xr_string> pins;
		for (u32 qi = 0; qi < work.size(); ++qi)
		{
			const int current = work[qi];
			const SNqNode& source = q.nodes[current];
			BuildPinOrder(source, pins);
			for (u32 visual_pin = 0; visual_pin < pins.size(); ++visual_pin)
			{
				const SNqPin* output = 0;
				for (u32 p = 0; p < source.out.size(); ++p)
					if (source.out[p].first == pins[visual_pin]) { output = &source.out[p]; break; }
				if (!output) continue;
				const float side = PinSide((int)visual_pin, (int)pins.size());
				for (u32 t = 0; t < output->second.size(); ++t)
				{
					const int target = q.NodeIndex(output->second[t].c_str());
					if (target < 0 || NqText::IsTrigger(q.nodes[target].kind.c_str())) continue;
					SLayoutNode& child = layout[target];
					if (child.layer < 0)
					{
						child.layer = layout[current].layer + 1;
						child.parent = current;
						child.root = layout[current].root;
						layout[current].children.push_back(target);
						work.push_back(target);
						order.push_back(target);
						max_layer = std::max(max_layer, child.layer);
					}
					if (child.parent == current)
					{
						child.side_sum += side;
						++child.side_count;
					}
				}
			}
		}
	};

	expand(queue);
	const int orphan_layer = roots.empty() ? 0 : max_layer + 1;
	for (u32 i = 0; i < count; ++i)
	{
		if (layout[i].layer >= 0) continue;
		layout[i].layer = orphan_layer;
		layout[i].root = (int)i;
		orphan_roots.push_back((int)i);
		order.push_back((int)i);
		queue.clear();
		queue.push_back((int)i);
		max_layer = std::max(max_layer, orphan_layer);
		expand(queue);
	}

	// Children are processed before parents, so each packed block uses complete descendant bounds.
	for (int oi = (int)order.size() - 1; oi >= 0; --oi)
	{
		const int index = order[oi];
		SLayoutNode& node = layout[index];
		xr_vector<int> left, middle, right;
		for (u32 c = 0; c < node.children.size(); ++c)
		{
			const int child_index = node.children[c];
			const SLayoutNode& child = layout[child_index];
			const float side = child.side_count ? child.side_sum / float(child.side_count) : 0.f;
			if (side < -0.001f) left.push_back(child_index);
			else if (side > 0.001f) right.push_back(child_index);
			else middle.push_back(child_index);
		}

		SBlock middle_block;
		if (middle.size() == 1)
		{
			SLayoutNode& child = layout[middle[0]];
			child.offset_x = 0.f;
			middle_block.min_x = child.min_x;
			middle_block.max_x = child.max_x;
		}
		else if (!middle.empty())
		{
			middle_block = PackBlock(layout, middle, kGapX);
			ShiftBlock(layout, middle, middle_block, -(middle_block.min_x + middle_block.max_x) * 0.5f);
		}

		SBlock left_block;
		if (!left.empty())
		{
			left_block = PackBlock(layout, left, kGapX);
			const float edge = middle.empty() ? -kGapX * 0.5f : middle_block.min_x - kGapX;
			ShiftBlock(layout, left, left_block, edge - left_block.max_x);
		}
		SBlock right_block;
		if (!right.empty())
		{
			right_block = PackBlock(layout, right, kGapX);
			const float edge = middle.empty() ? kGapX * 0.5f : middle_block.max_x + kGapX;
			ShiftBlock(layout, right, right_block, edge - right_block.min_x);
		}

		node.min_x = -node.w * 0.5f;
		node.max_x = node.w * 0.5f;
		if (!left.empty()) { node.min_x = std::min(node.min_x, left_block.min_x); node.max_x = std::max(node.max_x, left_block.max_x); }
		if (!middle.empty()) { node.min_x = std::min(node.min_x, middle_block.min_x); node.max_x = std::max(node.max_x, middle_block.max_x); }
		if (!right.empty()) { node.min_x = std::min(node.min_x, right_block.min_x); node.max_x = std::max(node.max_x, right_block.max_x); }
	}

	PackRoots(layout, roots);
	PackRoots(layout, orphan_roots);
	for (u32 i = 0; i < order.size(); ++i)
	{
		SLayoutNode& node = layout[order[i]];
		if (node.parent >= 0) node.center_x = layout[node.parent].center_x + node.offset_x;
	}

	xr_vector<float> row_height(max_layer + 1, 0.f);
	xr_vector<float> row_y(max_layer + 1, 0.f);
	for (u32 i = 0; i < count; ++i)
		row_height[layout[i].layer] = std::max(row_height[layout[i].layer], layout[i].h);
	float y = 0.f;
	for (int layer = 0; layer <= max_layer; ++layer)
	{
		row_y[layer] = Snap(y);
		y = row_y[layer] + row_height[layer] + kGapY;
	}
	for (u32 i = 0; i < count; ++i)
	{
		layout[i].x = Sane(Snap(layout[i].center_x - layout[i].w * 0.5f));
		layout[i].y = Sane(row_y[layout[i].layer]);
	}

	int moved = 0;
	if (only_missing)
	{
		bool any_existing = false;
		float max_y = -1e9f, min_missing_y = 1e9f;
		for (u32 i = 0; i < count; ++i)
		{
			if (q.nodes[i].has_pos) { any_existing = true; max_y = std::max(max_y, q.nodes[i].pos.y + layout[i].h); }
			else min_missing_y = std::min(min_missing_y, layout[i].y);
		}
		const float dy = any_existing ? Snap(max_y + kGapY - min_missing_y) : 0.f;
		for (u32 i = 0; i < count; ++i)
		{
			if (q.nodes[i].has_pos) continue;
			q.nodes[i].pos.set(layout[i].x, Sane(layout[i].y + dy));
			q.nodes[i].has_pos = true;
			++moved;
		}
		return moved;
	}
	for (u32 i = 0; i < count; ++i)
	{
		if (!q.nodes[i].has_pos || q.nodes[i].pos.x != layout[i].x || q.nodes[i].pos.y != layout[i].y) ++moved;
		q.nodes[i].pos.set(layout[i].x, layout[i].y);
		q.nodes[i].has_pos = true;
	}
	return moved;
}
