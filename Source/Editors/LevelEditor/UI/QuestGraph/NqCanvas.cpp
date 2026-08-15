#include "stdafx.h"
#include "NqCanvas.h"
#include "../../../XrECore/Editor/Nq/NqLayout.h"
#include "../../../XrECore/Editor/Nq/NqLua.h"
#include "../../../XrECore/Editor/Nq/NqUtil.h"

namespace
{
	// ten fixed zoom levels (docs par. 13.8); index 6 = 1:1
	const float kZoom[] = { 0.2f, 0.3f, 0.4f, 0.5f, 0.65f, 0.8f, 1.0f, 1.25f, 1.5f, 2.0f };
	const int	kZoomCount = sizeof(kZoom) / sizeof(kZoom[0]);
	const int	kZoomDefault = 6;

	const float kChipW		= 26.f;		// one chip in world units
	const float kChipPad	= 4.f;
	const float kPinR		= 5.f;
	const float kFontBase	= 14.f;		// world font size at zoom 1
	const float kLinkTangent = 48.f;

	IC ImU32 Col(u8 r, u8 g, u8 b, u8 a = 255) { return IM_COL32(r, g, b, a); }

	// family colour by kind prefix
	ImU32 FamilyColor(LPCSTR kind)
	{
		if (0 == strncmp(kind, "trigger.", 8))	return Col(58, 130, 72);
		if (0 == strncmp(kind, "dialog.", 7))	return Col(52, 96, 150);
		if (0 == strncmp(kind, "objective.", 10)) return Col(170, 105, 40);
		if (0 == strncmp(kind, "wait.", 5))		return Col(40, 120, 125);
		if (0 == strcmp(kind, "flow.end"))		return Col(150, 55, 55);
		if (0 == strncmp(kind, "flow.", 5))		return Col(95, 95, 105);
		return Col(90, 90, 90);
	}

	IC ImVec2 Add(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x + b.x, a.y + b.y); }
	IC ImVec2 Sub(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x - b.x, a.y - b.y); }
	IC bool InRect(const ImVec2& p, const ImVec2& a, const ImVec2& b) { return p.x >= a.x && p.y >= a.y && p.x <= b.x && p.y <= b.y; }
	IC float Snap(float v) { return floorf(v / NqLayout::kGrid + 0.5f) * NqLayout::kGrid; }

	// squared distance from p to segment ab
	float SegDist2(const ImVec2& p, const ImVec2& a, const ImVec2& b)
	{
		ImVec2 ab = Sub(b, a), ap = Sub(p, a);
		float l2 = ab.x * ab.x + ab.y * ab.y;
		float t = l2 > 0.f ? (ap.x * ab.x + ap.y * ab.y) / l2 : 0.f;
		t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
		ImVec2 c(a.x + ab.x * t, a.y + ab.y * t);
		return (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y);
	}

	// ImGui 1.88 has no ellipse primitives
	void AddOval(ImDrawList* dl, const ImVec2& c, float rx, float ry, ImU32 col, bool filled, float thickness)
	{
		const int segs = 40;
		dl->PathClear();
		for (int i = 0; i < segs; ++i)
		{
			float a = (float)i / (float)segs * 6.2831853f;
			dl->PathLineTo(ImVec2(c.x + cosf(a) * rx, c.y + sinf(a) * ry));
		}
		if (filled) dl->PathFillConvex(col);
		else		dl->PathStroke(col, ImDrawFlags_Closed, thickness);
	}

	ImVec2 Bezier(const ImVec2& p0, const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, float t)
	{
		float u = 1.f - t;
		float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
		return ImVec2(a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y);
	}
}

NqCanvas::NqCanvas(NqDoc* doc) : m_Doc(doc)
{
	m_Origin = m_Size = ImVec2(0, 0);
	m_Hovered = false;
	m_Panning = m_RmbMoved = false;
	m_Dragging = m_DragMoved = false;
	m_Marquee = false;
	m_Linking = m_PendingLink = false;
	m_RefusedAt = 0.0;
	m_ChipDragIndex = -1;
	m_WantFrameAll = m_WantFrameSel = m_WantRename = m_WantFocusAction = m_OpenAddAction = false;
	m_WantZoom = -1;
	m_ReachRevision = u32(-1);
	m_Filter[0] = 0;
	if (m_Doc->zoom_idx < 0 || m_Doc->zoom_idx >= kZoomCount) m_Doc->zoom_idx = kZoomDefault;
	// a freshly opened document frames itself once
	m_WantFrameAll = (m_Doc->view_cx == 0.f && m_Doc->view_cy == 0.f);
}

int   NqCanvas::ZoomLevels()			{ return kZoomCount; }
float NqCanvas::ZoomOf(int idx)			{ if (idx < 0) idx = 0; if (idx >= kZoomCount) idx = kZoomCount - 1; return kZoom[idx]; }

// an asked-for zoom outranks the zoom a pending frame request would pick, so
// quest_view with both `frame` and `zoom_level` shows the level that was asked
// for (and the answer it already reported)
void NqCanvas::SetZoom(int idx)
{
	if (idx < 0) idx = 0;
	if (idx >= kZoomCount) idx = kZoomCount - 1;
	m_Doc->zoom_idx = idx;
	m_WantZoom = idx;
}

void NqCanvas::RequestFrameAll()
{
	if (Measured()) FrameAll(); else m_WantFrameAll = true;
}

void NqCanvas::RequestFrameSel()
{
	if (Measured()) FrameSelection(); else m_WantFrameSel = true;
}

void NqCanvas::RequestFrameNode(LPCSTR id)
{
	if (Measured()) FrameNode(id); else m_WantFrameNode = id ? id : "";
}

ImVec2 NqCanvas::ToScreen(const ImVec2& w) const
{
	float z = Zoom();
	return ImVec2(m_Origin.x + m_Size.x * 0.5f + (w.x - m_Doc->view_cx) * z,
				  m_Origin.y + m_Size.y * 0.5f + (w.y - m_Doc->view_cy) * z);
}

ImVec2 NqCanvas::ToWorld(const ImVec2& s) const
{
	float z = Zoom();
	return ImVec2(m_Doc->view_cx + (s.x - m_Origin.x - m_Size.x * 0.5f) / z,
				  m_Doc->view_cy + (s.y - m_Origin.y - m_Size.y * 0.5f) / z);
}

void NqCanvas::ZoomBy(int delta, const ImVec2* anchor)
{
	int idx = m_Doc->zoom_idx + delta;
	if (idx < 0) idx = 0;
	if (idx >= kZoomCount) idx = kZoomCount - 1;
	if (idx == m_Doc->zoom_idx) return;
	if (anchor)
	{
		// keep the world point under the cursor in place
		ImVec2 w = ToWorld(*anchor);
		m_Doc->zoom_idx = idx;
		ImVec2 s = ToScreen(w);
		float z = Zoom();
		m_Doc->view_cx += (s.x - anchor->x) / z;
		m_Doc->view_cy += (s.y - anchor->y) / z;
	}
	else
		m_Doc->zoom_idx = idx;
}

void NqCanvas::Center(float wx, float wy)
{
	CancelFraming();
	m_Doc->view_cx = NqLayout::Sane(wx);
	m_Doc->view_cy = NqLayout::Sane(wy);
}

void NqCanvas::FrameRect(const ImVec2& wmin, const ImVec2& wmax)
{
	float w = wmax.x - wmin.x + 2 * NqLayout::kGapX, h = wmax.y - wmin.y + 2 * NqLayout::kGapY;
	if (w < 1.f) w = 1.f;
	if (h < 1.f) h = 1.f;
	int idx = kZoomCount - 1;
	if (m_Size.x > 0 && m_Size.y > 0)
		while (idx > 0 && (w * kZoom[idx] > m_Size.x || h * kZoom[idx] > m_Size.y)) --idx;
	if (idx > kZoomDefault) idx = kZoomDefault;		// framing never zooms in past 1:1
	m_Doc->zoom_idx = idx;
	m_Doc->view_cx = NqLayout::Sane((wmin.x + wmax.x) * 0.5f);
	m_Doc->view_cy = NqLayout::Sane((wmin.y + wmax.y) * 0.5f);
}

void NqCanvas::FrameAll()
{
	const SNqQuest& q = m_Doc->quest;
	if (q.nodes.empty()) { m_Doc->view_cx = m_Doc->view_cy = 0.f; m_Doc->zoom_idx = kZoomDefault; return; }
	ImVec2 mn(FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX);
	for (u32 i = 0; i < q.nodes.size(); ++i)
	{
		Fvector2 sz = NqLayout::NodeSize(q.nodes[i]);
		const Fvector2& p = q.nodes[i].pos;
		mn.x = _min(mn.x, p.x); mn.y = _min(mn.y, p.y);
		mx.x = _max(mx.x, p.x + sz.x); mx.y = _max(mx.y, p.y + sz.y);
	}
	FrameRect(mn, mx);
}

void NqCanvas::FrameSelection()
{
	if (m_Doc->selection.empty()) { FrameAll(); return; }
	ImVec2 mn(FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX);
	int cnt = 0;
	for (u32 i = 0; i < m_Doc->selection.size(); ++i)
	{
		const SNqNode* n = m_Doc->quest.FindNode(m_Doc->selection[i].c_str());
		if (!n) continue;
		Fvector2 sz = NqLayout::NodeSize(*n);
		mn.x = _min(mn.x, n->pos.x); mn.y = _min(mn.y, n->pos.y);
		mx.x = _max(mx.x, n->pos.x + sz.x); mx.y = _max(mx.y, n->pos.y + sz.y);
		++cnt;
	}
	if (cnt) FrameRect(mn, mx);
}

void NqCanvas::FrameNode(LPCSTR id)
{
	const SNqNode* n = m_Doc->quest.FindNode(id);
	if (!n) return;
	Fvector2 sz = NqLayout::NodeSize(*n);
	m_Doc->view_cx = n->pos.x + sz.x * 0.5f;
	m_Doc->view_cy = n->pos.y + sz.y * 0.5f;
	if (m_Doc->zoom_idx < kZoomDefault) m_Doc->zoom_idx = kZoomDefault;
	SelectOnly(id);
}

//------------------------------------------------------------------------------
// selection helpers
//------------------------------------------------------------------------------
bool NqCanvas::IsSelected(LPCSTR id) const
{
	for (u32 i = 0; i < m_Doc->selection.size(); ++i)
		if (m_Doc->selection[i] == id) return true;
	return false;
}

void NqCanvas::SelectOnly(LPCSTR id)
{
	m_Doc->selection.clear();
	if (id && id[0]) m_Doc->selection.push_back(id);
	m_Doc->sel_slot.clear();
}

void NqCanvas::ToggleSelected(LPCSTR id)
{
	for (u32 i = 0; i < m_Doc->selection.size(); ++i)
		if (m_Doc->selection[i] == id) { m_Doc->selection.erase(m_Doc->selection.begin() + i); return; }
	m_Doc->selection.push_back(id);
}

void NqCanvas::SelectAll()
{
	m_Doc->selection.clear();
	for (u32 i = 0; i < m_Doc->quest.nodes.size(); ++i)
		m_Doc->selection.push_back(m_Doc->quest.nodes[i].id);
}

void NqCanvas::DeleteSelection()
{
	if (m_Doc->selection.empty()) return;
	xr_string err;
	xr_vector<xr_string> ids = m_Doc->selection;
	if (m_Doc->RemoveNodes(ids, err))
	{
		m_Doc->selection.clear();
		m_Doc->sel_slot.clear();
	}
	else
		Msg("! [nq] delete: %s", err.c_str());
}

void NqCanvas::CopySelection()
{
	if (m_Doc->selection.empty()) return;
	xr_string text = "return {\n  nodes = {\n";
	for (u32 i = 0; i < m_Doc->selection.size(); ++i)
	{
		const SNqNode* n = m_Doc->quest.FindNode(m_Doc->selection[i].c_str());
		if (!n) continue;
		text += "    ";
		text += NqLua::WriteNode(*n, 4);
		text += ",\n";
	}
	text += "  },\n}\n";
	ImGui::SetClipboardText(text.c_str());
}

void NqCanvas::PasteClipboard()
{
	LPCSTR clip = ImGui::GetClipboardText();
	if (!clip || !clip[0]) return;
	SNqValue v; NqLua::SError err;
	if (!NqLua::ParseValue(clip, (u32)xr_strlen(clip), "clipboard", v, err)) { Msg("! [nq] paste: %s", err.message.c_str()); return; }
	const SNqValue* nodes = v.Get("nodes");
	if (!nodes || !nodes->IsTable()) { Msg("! [nq] paste: no nodes table"); return; }

	// read, remap ids, drop edges to nodes outside the fragment
	xr_vector<SNqNode> in;
	xr_vector<xr_string> shape;
	for (u32 i = 0; i < nodes->arr.size(); ++i)
	{
		SNqNode n;
		if (NqLua::NodeFromValue(nodes->arr[i], n, shape)) in.push_back(n);
	}
	if (in.empty()) return;
	ImVec2 anchor = m_Hovered ? ToWorld(ImGui::GetMousePos()) : ImVec2(m_Doc->view_cx, m_Doc->view_cy);
	InsertNodes(in, anchor, true);
}

void NqCanvas::DuplicateSelection()
{
	if (m_Doc->selection.empty()) return;
	// straight from the document: no system clipboard to clobber, and one undo
	// step instead of the copy+paste+move triple
	xr_vector<SNqNode> src;
	for (u32 i = 0; i < m_Doc->selection.size(); ++i)
		if (const SNqNode* n = m_Doc->quest.FindNode(m_Doc->selection[i].c_str())) src.push_back(*n);
	InsertNodes(src, ImVec2(NqLayout::kGrid * 2.f, NqLayout::kGrid * 2.f), false);
}

// remaps the ids against the document, drops edges that leave the fragment,
// places the nodes and selects them - all inside one NqDoc::Edit, so paste and
// duplicate are a single undo step each. `place` is the world anchor of the
// first node when `anchor`, otherwise a delta added to every position.
void NqCanvas::InsertNodes(xr_vector<SNqNode>& in, const ImVec2& place, bool anchor)
{
	if (in.empty()) return;
	xr_vector<std::pair<xr_string, xr_string> > remap;
	xr_vector<xr_string> taken;
	for (u32 i = 0; i < in.size(); ++i)
	{
		xr_string base = in[i].id.empty() ? "node" : in[i].id;
		xr_string nid = base;
		// unique against the document AND the already remapped fragment
		for (int k = 2; ; ++k)
		{
			bool clash = 0 != m_Doc->quest.FindNode(nid.c_str());
			for (u32 t = 0; !clash && t < taken.size(); ++t) if (taken[t] == nid) clash = true;
			if (!clash) break;
			nid = NqUtil::Format("%s%d", base.c_str(), k);
		}
		taken.push_back(nid);
		remap.push_back(std::make_pair(in[i].id, nid));
		in[i].id = nid;
	}
	// keep relative placement: shift the fragment so its first node lands on the anchor
	Fvector2 shift; shift.set(place.x, place.y);
	if (anchor)
	{
		shift.set(0.f, 0.f);
		if (in[0].has_pos) shift.set(place.x - in[0].pos.x, place.y - in[0].pos.y);
	}
	m_Doc->selection.clear();
	m_Doc->Edit([&](SNqQuest& q)
	{
		for (u32 i = 0; i < in.size(); ++i)
		{
			SNqNode n = in[i];
			for (u32 p = 0; p < n.out.size(); ++p)
			{
				xr_vector<xr_string> kept;
				for (u32 t = 0; t < n.out[p].second.size(); ++t)
					for (u32 r = 0; r < remap.size(); ++r)
						if (remap[r].first == n.out[p].second[t]) { kept.push_back(remap[r].second); break; }
				n.out[p].second = kept;
			}
			if (n.has_pos) { n.pos.x = Snap(n.pos.x + shift.x); n.pos.y = Snap(n.pos.y + shift.y); }
			else { n.has_pos = true; n.pos.set(Snap(place.x), Snap(place.y + i * (NqLayout::kNodeHeight + NqLayout::kGapY))); }
			q.nodes.push_back(n);
			m_Doc->selection.push_back(n.id);
		}
	});
}

//------------------------------------------------------------------------------
// geometry
//------------------------------------------------------------------------------
void NqCanvas::BuildGeometry()
{
	const SNqQuest& q = m_Doc->quest;
	m_Geom.resize(q.nodes.size());
	for (u32 i = 0; i < q.nodes.size(); ++i)
	{
		const SNqNode& n = q.nodes[i];
		SNodeGeom& g = m_Geom[i];
		Fvector2 sz = NqLayout::NodeSize(n);
		g.pos = ImVec2(n.pos.x, n.pos.y);
		g.size = ImVec2(sz.x, sz.y);
		g.trigger = NqText::IsTrigger(n.kind.c_str());
		g.strip_h = g.trigger ? 0.f : NqLayout::kChipStrip;
		g.enter_n = (int)n.on_enter.size();
		g.exit_n = (int)n.on_exit.size();
		g.pins.clear();
		const NqCatalog::SKind* k = NqCatalog::Find(n.kind.c_str());
		if (k)
		{
			if (k->pins_from_cases)
			{
				const SNqValue* cases = n.params.Get("cases");
				if (cases && cases->IsTable())
					for (u32 c = 0; c < cases->arr.size(); ++c)
					{
						xr_string nm = cases->arr[c].GetString("name");
						if (!nm.empty()) g.pins.push_back(nm);
					}
			}
			for (u32 p = 0; p < k->pins.size(); ++p)
				if (k->pins[p] != "cases") g.pins.push_back(k->pins[p]);
		}
		// pins present in the file but unknown to the catalog still get a handle
		for (u32 p = 0; p < n.out.size(); ++p)
		{
			bool have = false;
			for (u32 e = 0; e < g.pins.size(); ++e) if (g.pins[e] == n.out[p].first) { have = true; break; }
			if (!have) g.pins.push_back(n.out[p].first);
		}
	}
}

const NqCanvas::SNodeGeom* NqCanvas::GeomOf(LPCSTR id, int* index) const
{
	int i = m_Doc->quest.NodeIndex(id);
	if (index) *index = i;
	return (i >= 0 && i < (int)m_Geom.size()) ? &m_Geom[i] : 0;
}

ImVec2 NqCanvas::InputPin(const SNodeGeom& g) const
{
	return ImVec2(g.pos.x + g.size.x * 0.5f, g.pos.y);
}

ImVec2 NqCanvas::OutputPin(const SNodeGeom& g, int pin) const
{
	int n = (int)g.pins.size();
	float x = g.pos.x + g.size.x * float(pin + 1) / float(n + 1);
	return ImVec2(x, g.pos.y + g.size.y);
}

void NqCanvas::BuildLinks()
{
	m_Links.clear();
	const SNqQuest& q = m_Doc->quest;
	float z = Zoom();
	for (u32 i = 0; i < q.nodes.size(); ++i)
	{
		const SNqNode& n = q.nodes[i];
		const SNodeGeom& g = m_Geom[i];
		for (u32 p = 0; p < n.out.size(); ++p)
		{
			int pin = -1;
			for (u32 e = 0; e < g.pins.size(); ++e) if (g.pins[e] == n.out[p].first) { pin = (int)e; break; }
			if (pin < 0) continue;
			for (u32 t = 0; t < n.out[p].second.size(); ++t)
			{
				int to = q.NodeIndex(n.out[p].second[t].c_str());
				if (to < 0) continue;
				SLinkGeom l;
				l.from = (int)i; l.pin = n.out[p].first; l.to = to;
				l.p0 = ToScreen(OutputPin(g, pin));
				l.p3 = ToScreen(InputPin(m_Geom[to]));
				float d = _max(kLinkTangent * z, fabsf(l.p3.y - l.p0.y) * 0.4f);
				l.p1 = ImVec2(l.p0.x, l.p0.y + d);
				l.p2 = ImVec2(l.p3.x, l.p3.y - d);
				m_Links.push_back(l);
			}
		}
	}
}

int NqCanvas::HitNode(const ImVec2& s) const
{
	// topmost = last drawn = highest index
	for (int i = (int)m_Geom.size() - 1; i >= 0; --i)
	{
		const SNodeGeom& g = m_Geom[i];
		ImVec2 a = ToScreen(g.pos), b = ToScreen(Add(g.pos, g.size));
		if (InRect(s, a, b)) return i;
	}
	return -1;
}

int NqCanvas::HitPin(int node, const ImVec2& s) const
{
	if (node < 0) return -1;
	const SNodeGeom& g = m_Geom[node];
	float r = _max(kPinR * Zoom(), 6.f) + 3.f;
	for (u32 p = 0; p < g.pins.size(); ++p)
	{
		ImVec2 c = ToScreen(OutputPin(g, (int)p));
		if ((s.x - c.x) * (s.x - c.x) + (s.y - c.y) * (s.y - c.y) <= r * r) return (int)p;
	}
	return -1;
}

bool NqCanvas::HitChip(int node, const ImVec2& s, xr_string& slot, int& index, bool& plus) const
{
	if (node < 0) return false;
	const SNodeGeom& g = m_Geom[node];
	if (g.trigger || Zoom() < 0.5f) return false;
	float z = Zoom();
	ImVec2 tl = ToScreen(g.pos);
	float chip = kChipW * z, pad = kChipPad * z, strip = g.strip_h * z;
	for (int pass = 0; pass < 2; ++pass)
	{
		float y0 = pass == 0 ? tl.y : tl.y + g.size.y * z - strip;
		if (s.y < y0 || s.y > y0 + strip) continue;
		int count = pass == 0 ? g.enter_n : g.exit_n;
		float x = tl.x + pad;
		for (int i = 0; i <= count; ++i)
		{
			if (s.x >= x && s.x <= x + chip)
			{
				slot = pass == 0 ? "enter" : "exit";
				index = i; plus = (i == count);
				return true;
			}
			x += chip + pad;
		}
	}
	return false;
}

int NqCanvas::HitLink(const ImVec2& s) const
{
	float best = 36.f;		// squared pixels
	int hit = -1;
	for (u32 i = 0; i < m_Links.size(); ++i)
	{
		const SLinkGeom& l = m_Links[i];
		ImVec2 prev = l.p0;
		for (int k = 1; k <= 16; ++k)
		{
			ImVec2 cur = Bezier(l.p0, l.p1, l.p2, l.p3, k / 16.f);
			float d = SegDist2(s, prev, cur);
			if (d < best) { best = d; hit = (int)i; }
			prev = cur;
		}
	}
	return hit;
}

void NqCanvas::EnsureReachable()
{
	if (m_ReachRevision == m_Doc->revision && m_Reachable.size() == m_Doc->quest.nodes.size()) return;
	m_Doc->quest.Reachable(m_Reachable);
	m_ReachRevision = m_Doc->revision;
}

int NqCanvas::Severity(LPCSTR node_id) const
{
	int sev = 0;
	for (u32 i = 0; i < m_Doc->problems.size(); ++i)
		if (m_Doc->problems[i].node_id == node_id)
			sev = _max(sev, m_Doc->problems[i].IsError() ? 2 : 1);
	return sev;
}

void NqCanvas::ProblemTooltip(LPCSTR node_id) const
{
	ImGui::BeginTooltip();
	for (u32 i = 0; i < m_Doc->problems.size(); ++i)
		if (m_Doc->problems[i].node_id == node_id)
			ImGui::TextUnformatted(m_Doc->problems[i].Text().c_str());
	ImGui::EndTooltip();
}

void NqCanvas::ChipTooltip(const SNqAction& a) const
{
	const NqCatalog::SKind* k = NqCatalog::Find(a.kind.c_str());
	ImGui::BeginTooltip();
	ImGui::TextUnformatted(k ? k->title.c_str() : a.kind.c_str());
	ImGui::TextDisabled("%s", a.kind.c_str());
	xr_string sum = NqText::ValueSummary(a.params, 96);
	if (!sum.empty()) ImGui::TextUnformatted(sum.c_str());
	ImGui::EndTooltip();
}

xr_string NqCanvas::NodeTitle(const SNqNode& n) const
{
	const NqCatalog::SKind* k = NqCatalog::Find(n.kind.c_str());
	return k ? k->title : n.kind;
}

xr_string NqCanvas::NodeSummary(const SNqNode& n) const
{
	// the one thing an author wants to see on the box: the text, the NPC, the target...
	static LPCSTR keys[] = { "text", "npc", "target", "section", "place", "duration", "quest", "task", "name", "status", 0 };
	for (int i = 0; keys[i]; ++i)
	{
		const SNqValue* v = n.params.Get(keys[i]);
		if (!v || v->IsNil()) continue;
		xr_string s = (0 == strcmp(keys[i], "text")) ? NqText::Preview(*v) : NqText::RefSummary(*v);
		if (s.empty()) s = NqText::ValueSummary(*v, 40);
		return NqUtil::ClipUtf8(s, 40);
	}
	return "";
}

ImU32 NqCanvas::NodeColor(const SNqNode& n, bool) const
{
	return FamilyColor(n.kind.c_str());
}

//------------------------------------------------------------------------------
// drawing
//------------------------------------------------------------------------------
void NqCanvas::DrawGrid(ImDrawList* dl)
{
	float z = Zoom();
	float step = NqLayout::kGrid * 4.f * z;
	while (step < 24.f) step *= 2.f;
	// the pattern is anchored on the projection of the world origin (fmodf of a
	// negative offset shifts it)
	ImVec2 o = ToScreen(ImVec2(0, 0));
	float sx = o.x - floorf((o.x - m_Origin.x) / step) * step;
	float sy = o.y - floorf((o.y - m_Origin.y) / step) * step;
	if (!_finite(sx) || !_finite(sy) || _isnan(sx) || _isnan(sy)) return;
	// count the lines instead of accumulating a float: far enough from the
	// origin `x += step` stops advancing and the loop would never end
	const int nx = int(m_Size.x / step) + 2, ny = int(m_Size.y / step) + 2;
	const int kMaxLines = 4096;
	if (nx < 1 || ny < 1 || nx > kMaxLines || ny > kMaxLines) return;
	ImU32 col = Col(255, 255, 255, 14);
	const float x1 = m_Origin.x + m_Size.x, y1 = m_Origin.y + m_Size.y;
	for (int i = 0; i < nx; ++i)
	{
		const float x = sx + i * step;
		if (x >= x1) break;
		if (x >= m_Origin.x) dl->AddLine(ImVec2(x, m_Origin.y), ImVec2(x, y1), col);
	}
	for (int i = 0; i < ny; ++i)
	{
		const float y = sy + i * step;
		if (y >= y1) break;
		if (y >= m_Origin.y) dl->AddLine(ImVec2(m_Origin.x, y), ImVec2(x1, y), col);
	}
}

void NqCanvas::DrawLinks(ImDrawList* dl)
{
	float z = Zoom();
	float th = _max(1.f, 2.f * z);
	for (u32 i = 0; i < m_Links.size(); ++i)
	{
		const SLinkGeom& l = m_Links[i];
		bool sel = IsSelected(m_Doc->quest.nodes[l.from].id.c_str()) || IsSelected(m_Doc->quest.nodes[l.to].id.c_str());
		ImU32 col = sel ? Col(255, 220, 120) : Col(200, 200, 205, 200);
		dl->AddBezierCubic(l.p0, l.p1, l.p2, l.p3, col, th, 24);
		// arrow head at the target
		float a = _max(4.f, 7.f * z);
		dl->AddTriangleFilled(l.p3, ImVec2(l.p3.x - a * 0.6f, l.p3.y - a), ImVec2(l.p3.x + a * 0.6f, l.p3.y - a), col);
	}
}

void NqCanvas::DrawChipStrip(ImDrawList* dl, int index, LPCSTR slot, const ImVec2& tl, float w)
{
	const SNqNode& n = m_Doc->quest.nodes[index];
	const xr_vector<SNqAction>* acts = n.SlotC(slot);
	float z = Zoom();
	float chip = kChipW * z, pad = kChipPad * z, h = NqLayout::kChipStrip * z;
	xr_string prefix = xr_string(slot) + ":";
	float x = tl.x + pad;
	int count = acts ? (int)acts->size() : 0;
	ImFont* font = ImGui::GetFont();
	float fs = kFontBase * z * 0.85f;
	for (int i = 0; i <= count; ++i)
	{
		ImVec2 a(x, tl.y + pad * 0.5f), b(x + chip, tl.y + h - pad * 0.5f);
		bool plus = (i == count);
		bool selected = !plus && m_Doc->sel_slot == prefix + NqUtil::Format("%d", i);
		bool dragging = !plus && m_ChipDragIndex == i && m_ChipDragSlot == slot && m_ChipDragNode == n.id;
		ImU32 bg = plus ? Col(255, 255, 255, 28) : (selected ? Col(255, 210, 90, 220) : Col(255, 255, 255, 60));
		if (dragging) bg = Col(120, 200, 255, 220);
		dl->AddRectFilled(a, b, bg, 3.f * z);
		char label[16];
		if (plus) xr_strcpy(label, "+"); else xr_sprintf(label, "%d", i + 1);
		ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
		dl->AddText(font, fs, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), plus ? Col(230, 230, 230, 160) : Col(20, 20, 20), label);
		x += chip + pad;
		if (x > tl.x + w) break;
	}
}

void NqCanvas::DrawNode(ImDrawList* dl, int index)
{
	const SNqNode& n = m_Doc->quest.nodes[index];
	const SNodeGeom& g = m_Geom[index];
	float z = Zoom();
	ImVec2 a = ToScreen(g.pos), b = ToScreen(Add(g.pos, g.size));
	// off-screen culling
	if (b.x < m_Origin.x || b.y < m_Origin.y || a.x > m_Origin.x + m_Size.x || a.y > m_Origin.y + m_Size.y) return;

	bool selected = IsSelected(n.id.c_str());
	bool reachable = index < (int)m_Reachable.size() ? m_Reachable[index] : true;
	ImU32 fam = NodeColor(n, selected);
	if (!reachable) fam = (fam & 0x00FFFFFF) | 0x60000000;
	ImU32 body = reachable ? Col(46, 46, 50, 240) : Col(46, 46, 50, 140);
	ImU32 outline = selected ? Col(255, 255, 255) : Col(20, 20, 20, 200);
	float round = 6.f * z;
	ImFont* font = ImGui::GetFont();
	float fs = kFontBase * z;

	if (g.trigger)
	{
		// oval "T · <kind>"
		ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
		float rx = (b.x - a.x) * 0.5f, ry = (b.y - a.y) * 0.5f;
		AddOval(dl, c, rx, ry, fam, true, 0.f);
		AddOval(dl, c, rx, ry, outline, false, selected ? 2.5f : 1.5f);
		if (z >= 0.3f)
		{
			xr_string label = "T  " + NodeTitle(n);
			ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, label.c_str());
			dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y - 1.f), Col(255, 255, 255), label.c_str());
			ImVec2 is = font->CalcTextSizeA(fs * 0.8f, FLT_MAX, 0.f, n.id.c_str());
			dl->AddText(font, fs * 0.8f, ImVec2(c.x - is.x * 0.5f, c.y + 1.f), Col(220, 220, 220, 200), n.id.c_str());
		}
	}
	else
	{
		float strip = g.strip_h * z;
		dl->AddRectFilled(a, b, body, round);
		// coloured body band between the strips
		dl->AddRectFilled(ImVec2(a.x, a.y + strip), ImVec2(b.x, b.y - strip), fam);
		if (z >= 0.5f)
		{
			DrawChipStrip(dl, index, "enter", a, b.x - a.x);
			DrawChipStrip(dl, index, "exit", ImVec2(a.x, b.y - strip), b.x - a.x);
		}
		else
		{
			// LOD: strips as thin bars carrying the counts
			ImU32 bar = Col(255, 255, 255, 40);
			dl->AddRectFilled(a, ImVec2(b.x, a.y + strip), bar, round, ImDrawFlags_RoundCornersTop);
			dl->AddRectFilled(ImVec2(a.x, b.y - strip), b, bar, round, ImDrawFlags_RoundCornersBottom);
		}
		dl->AddRect(a, b, outline, round, 0, selected ? 2.5f : 1.f);
		if (z >= 0.3f)
		{
			float tx = a.x + 8.f * z, ty = a.y + strip + 5.f * z;
			xr_string title = NodeTitle(n);
			dl->AddText(font, fs, ImVec2(tx, ty), Col(255, 255, 255), title.c_str());
			ImVec2 ts = font->CalcTextSizeA(fs * 0.8f, FLT_MAX, 0.f, n.id.c_str());
			dl->AddText(font, fs * 0.8f, ImVec2(b.x - ts.x - 8.f * z, ty + 2.f * z), Col(230, 230, 230, 170), n.id.c_str());
			if (z >= 0.5f)
			{
				xr_string sum = NodeSummary(n);
				if (!sum.empty())
					dl->AddText(font, fs * 0.9f, ImVec2(tx, ty + fs + 4.f * z), Col(235, 235, 235, 220), sum.c_str());
				// `once` absent from the file means the catalog default applies
				const NqCatalog::SKind* nk = NqCatalog::Find(n.kind.c_str());
				if (n.once_set ? n.once : (nk && nk->once_default))
					dl->AddText(font, fs * 0.8f, ImVec2(tx, b.y - strip - fs), Col(255, 230, 150, 200), "once");
			}
		}
	}

	// output pins with labels
	float pr = _max(3.f, kPinR * z);
	for (u32 p = 0; p < g.pins.size(); ++p)
	{
		ImVec2 c = ToScreen(OutputPin(g, (int)p));
		bool hot = m_Linking && m_LinkFrom == n.id && m_LinkPin == g.pins[p];
		dl->AddCircleFilled(c, pr, hot ? Col(255, 220, 120) : Col(230, 230, 230), 12);
		dl->AddCircle(c, pr, Col(20, 20, 20), 12, 1.f);
		if (z >= 0.5f && (g.pins.size() > 1 || g.pins[p] != "next"))
		{
			ImVec2 ts = font->CalcTextSizeA(fs * 0.75f, FLT_MAX, 0.f, g.pins[p].c_str());
			dl->AddText(font, fs * 0.75f, ImVec2(c.x - ts.x * 0.5f, c.y + pr + 1.f), Col(220, 220, 220, 210), g.pins[p].c_str());
		}
	}
	// input pin
	if (!g.trigger)
	{
		ImVec2 c = ToScreen(InputPin(g));
		dl->AddCircleFilled(c, pr, Col(200, 200, 200), 12);
		dl->AddCircle(c, pr, Col(20, 20, 20), 12, 1.f);
	}
	// problem badge
	int sev = Severity(n.id.c_str());
	if (sev)
	{
		float r = _max(4.f, 6.f * z);
		ImVec2 c(b.x - r - 2.f, a.y + r + 2.f);
		dl->AddCircleFilled(c, r, sev == 2 ? Col(230, 60, 60) : Col(240, 190, 50), 12);
		if (m_Hovered && InRect(ImGui::GetMousePos(), ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r)))
			ProblemTooltip(n.id.c_str());
	}
}

void NqCanvas::DrawMarquee(ImDrawList* dl)
{
	if (!m_Marquee) return;
	ImVec2 m = ImGui::GetMousePos();
	ImVec2 a(_min(m.x, m_MarqueeFrom.x), _min(m.y, m_MarqueeFrom.y)), b(_max(m.x, m_MarqueeFrom.x), _max(m.y, m_MarqueeFrom.y));
	dl->AddRectFilled(a, b, Col(120, 170, 255, 40));
	dl->AddRect(a, b, Col(120, 170, 255, 200));
}

void NqCanvas::DrawLinking(ImDrawList* dl)
{
	if (!m_Linking) return;
	int fi = -1;
	const SNodeGeom* g = GeomOf(m_LinkFrom.c_str(), &fi);
	if (!g) { m_Linking = false; return; }
	int pin = -1;
	for (u32 p = 0; p < g->pins.size(); ++p) if (g->pins[p] == m_LinkPin) { pin = (int)p; break; }
	if (pin < 0) { m_Linking = false; return; }
	ImVec2 p0 = ToScreen(OutputPin(*g, pin)), p3 = ImGui::GetMousePos();
	float d = _max(kLinkTangent * Zoom(), fabsf(p3.y - p0.y) * 0.4f);
	dl->AddBezierCubic(p0, ImVec2(p0.x, p0.y + d), ImVec2(p3.x, p3.y - d), p3, Col(255, 220, 120), 2.f, 24);
}

//------------------------------------------------------------------------------
// input
//------------------------------------------------------------------------------
void NqCanvas::HandleInput()
{
	ImGuiIO& io = ImGui::GetIO();
	ImVec2 mouse = ImGui::GetMousePos();
	bool hovered = m_Hovered;

	// wheel: zoom around the cursor
	if (hovered && io.MouseWheel != 0.f)
		ZoomBy(io.MouseWheel > 0.f ? 1 : -1, &mouse);

	// right button: pan while dragging, context menu on a click
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { m_Panning = true; m_RmbMoved = false; m_RmbDown = mouse; }
	if (m_Panning && ImGui::IsMouseDown(ImGuiMouseButton_Right))
	{
		ImVec2 d = io.MouseDelta;
		if (d.x != 0.f || d.y != 0.f)
		{
			if (fabsf(mouse.x - m_RmbDown.x) + fabsf(mouse.y - m_RmbDown.y) > 3.f) m_RmbMoved = true;
			if (m_RmbMoved) { float z = Zoom(); m_Doc->view_cx -= d.x / z; m_Doc->view_cy -= d.y / z; }
		}
	}
	if (m_Panning && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
	{
		m_Panning = false;
		if (!m_RmbMoved && hovered) OpenContextMenus();
	}
	// middle button pans too
	if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
	{
		float z = Zoom();
		m_Doc->view_cx -= io.MouseDelta.x / z; m_Doc->view_cy -= io.MouseDelta.y / z;
	}

	// left button
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		int node = HitNode(mouse);
		int pin = HitPin(node, mouse);
		if (node < 0)
		{
			// pins stick out below the box: test every node's pins
			for (int i = (int)m_Geom.size() - 1; i >= 0 && pin < 0; --i)
			{
				int p = HitPin(i, mouse);
				if (p >= 0) { node = i; pin = p; }
			}
		}
		if (pin >= 0)
		{
			m_Linking = true;
			m_LinkFrom = m_Doc->quest.nodes[node].id;
			m_LinkPin = m_Geom[node].pins[pin];
		}
		else if (node >= 0)
		{
			const xr_string& id = m_Doc->quest.nodes[node].id;
			xr_string slot; int idx = 0; bool plus = false;
			if (HitChip(node, mouse, slot, idx, plus))
			{
				SelectOnly(id.c_str());
				if (plus)
				{
					m_CtxNode = id;
					m_ChipDragSlot = slot;
					m_OpenAddAction = true;
				}
				else
				{
					m_Doc->sel_slot = slot + ":" + NqUtil::Format("%d", idx);
					m_ChipDragNode = id; m_ChipDragSlot = slot; m_ChipDragIndex = idx;
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_WantFocusAction = true;
				}
			}
			else
			{
				if (io.KeyCtrl) ToggleSelected(id.c_str());
				else if (!IsSelected(id.c_str())) SelectOnly(id.c_str());
				else m_Doc->sel_slot.clear();
				// start dragging the whole selection
				m_Dragging = true; m_DragMoved = false; m_DragStartMouse = mouse;
				m_DragStart.clear();
				for (u32 i = 0; i < m_Doc->selection.size(); ++i)
				{
					const SNqNode* n = m_Doc->quest.FindNode(m_Doc->selection[i].c_str());
					if (n) m_DragStart.push_back(std::make_pair(n->id, ImVec2(n->pos.x, n->pos.y)));
				}
			}
		}
		else
		{
			int link = HitLink(mouse);
			if (link >= 0 && io.KeyAlt)
			{
				const SLinkGeom& l = m_Links[link];
				xr_string err;
				m_Doc->Disconnect(m_Doc->quest.nodes[l.from].id.c_str(), l.pin.c_str(), m_Doc->quest.nodes[l.to].id.c_str(), err);
			}
			else
			{
				// EndMarquee only ever adds; ctrl is the difference, and it is
				// expressed by keeping (or dropping) the current selection here
				m_Marquee = true; m_MarqueeFrom = mouse;
				if (!io.KeyCtrl) { m_Doc->selection.clear(); m_Doc->sel_slot.clear(); }
			}
		}
	}

	// dragging nodes
	if (m_Dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		ImVec2 d = Sub(mouse, m_DragStartMouse);
		if (fabsf(d.x) + fabsf(d.y) > 2.f) m_DragMoved = true;
		if (m_DragMoved)
		{
			float z = Zoom();
			for (u32 i = 0; i < m_DragStart.size(); ++i)
			{
				SNqNode* n = m_Doc->quest.FindNode(m_DragStart[i].first.c_str());
				if (n) { n->pos.x = m_DragStart[i].second.x + d.x / z; n->pos.y = m_DragStart[i].second.y + d.y / z; n->has_pos = true; }
			}
		}
	}
	if (m_Dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) EndDrag();

	// chip drag (reorder inside a strip)
	if (m_ChipDragIndex >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		int node = HitNode(mouse);
		xr_string slot; int idx = 0; bool plus = false;
		if (node >= 0 && m_Doc->quest.nodes[node].id == m_ChipDragNode && HitChip(node, mouse, slot, idx, plus) && slot == m_ChipDragSlot && !plus && idx != m_ChipDragIndex)
		{
			int from = m_ChipDragIndex, to = idx;
			xr_string nid = m_ChipDragNode, s = slot;
			m_Doc->Edit([&](SNqQuest& q)
			{
				SNqNode* n = q.FindNode(nid.c_str());
				if (!n) return;
				xr_vector<SNqAction>& v = n->Slot(s.c_str());
				if (from < (int)v.size() && to < (int)v.size())
				{
					SNqAction a = v[from];
					v.erase(v.begin() + from);
					v.insert(v.begin() + to, a);
				}
			});
			m_Doc->sel_slot = s + ":" + NqUtil::Format("%d", to);
		}
		m_ChipDragIndex = -1; m_ChipDragNode.clear();
	}

	if (m_Linking && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) EndLink();
	if (m_Marquee && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) EndMarquee();

	// hover feedback: chips
	if (hovered && !m_Dragging && !m_Linking && !m_Marquee)
	{
		int node = HitNode(mouse);
		xr_string slot; int idx = 0; bool plus = false;
		if (node >= 0 && HitChip(node, mouse, slot, idx, plus))
		{
			if (plus) ImGui::SetTooltip("add %s action", slot.c_str());
			else
			{
				const xr_vector<SNqAction>* acts = m_Doc->quest.nodes[node].SlotC(slot.c_str());
				if (acts && idx < (int)acts->size()) ChipTooltip((*acts)[idx]);
			}
		}
		else if (node >= 0 && HitPin(node, mouse) < 0 && Zoom() < 0.5f)
		{
			const SNqNode& n = m_Doc->quest.nodes[node];
			ImGui::SetTooltip("%s\n%s\n%s", n.id.c_str(), NodeTitle(n).c_str(), NodeSummary(n).c_str());
		}
	}
}

void NqCanvas::EndDrag()
{
	m_Dragging = false;
	if (m_DragMoved)
	{
		// commit once: restore the start positions, snapshot, then apply snapped
		xr_vector<std::pair<xr_string, ImVec2> > moved;
		for (u32 i = 0; i < m_DragStart.size(); ++i)
		{
			SNqNode* n = m_Doc->quest.FindNode(m_DragStart[i].first.c_str());
			if (!n) continue;
			moved.push_back(std::make_pair(n->id, ImVec2(Snap(n->pos.x), Snap(n->pos.y))));
			n->pos.x = m_DragStart[i].second.x; n->pos.y = m_DragStart[i].second.y;
		}
		m_Doc->Edit([&](SNqQuest& q)
		{
			for (u32 i = 0; i < moved.size(); ++i)
			{
				SNqNode* n = q.FindNode(moved[i].first.c_str());
				if (n) { n->pos.x = moved[i].second.x; n->pos.y = moved[i].second.y; n->has_pos = true; }
			}
		});
	}
	m_DragStart.clear();
}

void NqCanvas::EndLink()
{
	m_Linking = false;
	m_Refused.clear();
	ImVec2 mouse = ImGui::GetMousePos();
	int node = HitNode(mouse);
	if (node >= 0)
	{
		const SNqNode& to = m_Doc->quest.nodes[node];
		// a refused drop used to vanish without a word; say why instead
		if (to.id == m_LinkFrom)
			m_Refused = "a node cannot link to itself";
		else if (NqText::IsTrigger(to.kind.c_str()))
			m_Refused = NqUtil::Format("'%s' is a trigger - triggers have no input", to.id.c_str());
		else
		{
			xr_string err;
			if (!m_Doc->Connect(m_LinkFrom.c_str(), m_LinkPin.c_str(), to.id.c_str(), err) && !err.empty())
				Msg("! [nq] connect: %s", err.c_str());
		}
		if (!m_Refused.empty()) { Msg("~ [nq] link: %s", m_Refused.c_str()); m_RefusedAt = ImGui::GetTime(); }
	}
	else if (m_Hovered)
	{
		// dropped on empty space: create a node there and connect it
		m_PendingLink = true;
		m_MenuWorld = ToWorld(mouse);
		m_Filter[0] = 0;
		ImGui::OpenPopup("nq_add_node");
	}
}

void NqCanvas::EndMarquee()
{
	m_Marquee = false;
	ImVec2 m = ImGui::GetMousePos();
	ImVec2 a(_min(m.x, m_MarqueeFrom.x), _min(m.y, m_MarqueeFrom.y)), b(_max(m.x, m_MarqueeFrom.x), _max(m.y, m_MarqueeFrom.y));
	if (b.x - a.x < 2.f && b.y - a.y < 2.f) return;
	for (u32 i = 0; i < m_Geom.size(); ++i)
	{
		ImVec2 na = ToScreen(m_Geom[i].pos), nb = ToScreen(Add(m_Geom[i].pos, m_Geom[i].size));
		bool inter = !(nb.x < a.x || na.x > b.x || nb.y < a.y || na.y > b.y);
		if (inter && !IsSelected(m_Doc->quest.nodes[i].id.c_str())) m_Doc->selection.push_back(m_Doc->quest.nodes[i].id);
	}
}

void NqCanvas::HandleKeys()
{
	if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
	// Del / Ctrl+A / F2 belong to the canvas only while the canvas is the part
	// of the tab being worked in - the inspector next to it has its own lists
	if (!m_Hovered && !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) return;
	if (ImGui::GetIO().WantTextInput) return;		// a text field owns the keyboard
	ImGuiIO& io = ImGui::GetIO();
	if (ImGui::IsKeyPressed(ImGuiKey_Delete))						DeleteSelection();
	if (ImGui::IsKeyPressed(ImGuiKey_Home))							FrameAll();
	if (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeyCtrl)				FrameSelection();
	if (ImGui::IsKeyPressed(ImGuiKey_F2))							m_WantRename = true;
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))				SelectAll();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))				CopySelection();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V))				PasteClipboard();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D))				DuplicateSelection();
	if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) m_Doc->Undo();
	if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))) m_Doc->Redo();
	if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { m_Linking = false; m_Marquee = false; }
}

//------------------------------------------------------------------------------
// context menus
//------------------------------------------------------------------------------
void NqCanvas::OpenContextMenus()
{
	ImVec2 mouse = ImGui::GetMousePos();
	int node = HitNode(mouse);
	m_MenuWorld = ToWorld(mouse);
	if (node >= 0)
	{
		m_CtxNode = m_Doc->quest.nodes[node].id;
		if (!IsSelected(m_CtxNode.c_str())) SelectOnly(m_CtxNode.c_str());
		xr_string slot; int idx = 0; bool plus = false;
		if (HitChip(node, mouse, slot, idx, plus) && !plus)
		{
			m_ChipDragSlot = slot; m_ChipDragIndex = -1;
			m_Doc->sel_slot = slot + ":" + NqUtil::Format("%d", idx);
			ImGui::OpenPopup("nq_chip_ctx");
		}
		else
			ImGui::OpenPopup("nq_node_ctx");
		return;
	}
	int link = HitLink(mouse);
	if (link >= 0)
	{
		const SLinkGeom& l = m_Links[link];
		m_CtxLinkFrom = m_Doc->quest.nodes[l.from].id; m_CtxLinkPin = l.pin; m_CtxLinkTo = m_Doc->quest.nodes[l.to].id;
		ImGui::OpenPopup("nq_link_ctx");
		return;
	}
	m_PendingLink = false;
	m_Filter[0] = 0;
	ImGui::OpenPopup("nq_add_node");
}

void NqCanvas::CreateNode(const NqCatalog::SKind& k, const ImVec2& world, bool connect_pending)
{
	SNqNode n;
	// short id from the kind's last segment
	LPCSTR dot = strrchr(k.id.c_str(), '.');
	xr_string base = dot ? xr_string(dot + 1) : k.id;
	if (base == "start" || base == "when") base = "trigger";
	n.id = m_Doc->FreeNodeId(base.c_str());
	n.kind = k.id;
	n.params = SNqValue::Table();
	n.pos.set(Snap(world.x), Snap(world.y));
	n.has_pos = true;
	if (n.id.empty() || m_Doc->quest.FindNode(n.id.c_str())) { Msg("! [nq] add node: node '%s' already exists", n.id.c_str()); return; }
	// the node and the edge that asked for it are one gesture, so they are one
	// undo step: AddNode + Connect would take two Ctrl+Z to undo
	const xr_string from = m_LinkFrom, pin = m_LinkPin;
	const bool wire = connect_pending && !from.empty();
	m_Doc->Edit([&](SNqQuest& q)
	{
		q.nodes.push_back(n);
		if (wire)
			if (SNqNode* f = q.FindNode(from.c_str())) f->Connect(pin.c_str(), n.id.c_str());
	});
	SelectOnly(n.id.c_str());
	m_WantRename = false;
}

void NqCanvas::DrawAddNodeMenu(LPCSTR popup)
{
	if (!ImGui::BeginPopup(popup)) { if (0 == strcmp(popup, "nq_add_node")) m_PendingLink = false; return; }
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint("##nq_filter", "search kinds", m_Filter, sizeof(m_Filter));
	ImGui::Separator();
	xr_vector<const NqCatalog::SKind*> kinds;
	NqCatalog::KindsFor(NqCatalog::useTrigger | NqCatalog::useMain, kinds);
	xr_string flt = NqUtil::Trim(m_Filter);
	xr_string cur_group;
	bool any = false;
	for (u32 i = 0; i < kinds.size(); ++i)
	{
		const NqCatalog::SKind& k = *kinds[i];
		if (!flt.empty() && !strstr(k.id.c_str(), flt.c_str()) && !strstr(k.title.c_str(), flt.c_str())) continue;
		if (k.group != cur_group)
		{
			if (any) ImGui::Separator();
			ImGui::TextDisabled("%s", k.group.c_str());
			cur_group = k.group;
		}
		any = true;
		xr_string label = k.title + "##" + k.id;
		if (ImGui::MenuItem(label.c_str(), k.id.c_str()))
		{
			CreateNode(k, m_MenuWorld, m_PendingLink);
			m_PendingLink = false;
			ImGui::CloseCurrentPopup();
		}
		if (ImGui::IsItemHovered() && !k.desc.empty()) ImGui::SetTooltip("%s", k.desc.c_str());
	}
	if (!any) ImGui::TextDisabled("no kinds match");
	ImGui::EndPopup();
}

void NqCanvas::DrawContextMenus()
{
	// popups requested from inside other popups open here, at the canvas level
	if (m_OpenAddAction) { m_OpenAddAction = false; m_Filter[0] = 0; ImGui::OpenPopup("nq_add_action"); }
	DrawAddNodeMenu("nq_add_node");

	if (ImGui::BeginPopup("nq_node_ctx"))
	{
		if (ImGui::MenuItem("Rename", "F2"))			m_WantRename = true;
		if (ImGui::MenuItem("Duplicate", "Ctrl+D"))		DuplicateSelection();
		if (ImGui::MenuItem("Copy", "Ctrl+C"))			CopySelection();
		if (ImGui::MenuItem("Add on_enter action..."))	{ m_ChipDragSlot = "enter"; m_OpenAddAction = true; }
		if (ImGui::MenuItem("Add on_exit action..."))	{ m_ChipDragSlot = "exit"; m_OpenAddAction = true; }
		if (ImGui::MenuItem("Disconnect outputs"))
		{
			xr_string id = m_CtxNode;
			m_Doc->Edit([&](SNqQuest& q) { SNqNode* n = q.FindNode(id.c_str()); if (n) n->out.clear(); });
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Delete", "Del"))			DeleteSelection();
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("nq_add_action"))
	{
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::InputTextWithHint("##nq_afilter", "search actions", m_Filter, sizeof(m_Filter));
		ImGui::Separator();
		xr_vector<const NqCatalog::SKind*> kinds;
		NqCatalog::KindsFor(NqCatalog::useExtra, kinds);
		xr_string flt = NqUtil::Trim(m_Filter);
		xr_string cur_group;
		for (u32 i = 0; i < kinds.size(); ++i)
		{
			const NqCatalog::SKind& k = *kinds[i];
			if (!flt.empty() && !strstr(k.id.c_str(), flt.c_str()) && !strstr(k.title.c_str(), flt.c_str())) continue;
			if (k.group != cur_group) { ImGui::TextDisabled("%s", k.group.c_str()); cur_group = k.group; }
			xr_string label = k.title + "##" + k.id;
			if (ImGui::MenuItem(label.c_str(), k.id.c_str()))
			{
				xr_string id = m_CtxNode, slot = m_ChipDragSlot, kind = k.id;
				int index = -1;
				m_Doc->Edit([&](SNqQuest& q)
				{
					SNqNode* n = q.FindNode(id.c_str());
					if (!n) return;
					SNqAction a; a.kind = kind; a.params = SNqValue::Table();
					xr_vector<SNqAction>& v = n->Slot(slot.c_str());
					v.push_back(a);
					index = (int)v.size() - 1;
				});
				if (index >= 0) { m_Doc->sel_slot = slot + ":" + NqUtil::Format("%d", index); m_WantFocusAction = true; }
				ImGui::CloseCurrentPopup();
			}
			if (ImGui::IsItemHovered() && !k.desc.empty()) ImGui::SetTooltip("%s", k.desc.c_str());
		}
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("nq_chip_ctx"))
	{
		xr_string slot; int index = -1;
		{
			LPCSTR s = m_Doc->sel_slot.c_str();
			LPCSTR c = strchr(s, ':');
			if (c) { slot.assign(s, c - s); index = atoi(c + 1); }
		}
		if (ImGui::MenuItem("Duplicate action") && index >= 0)
		{
			xr_string id = m_CtxNode;
			m_Doc->Edit([&](SNqQuest& q)
			{
				SNqNode* n = q.FindNode(id.c_str()); if (!n) return;
				xr_vector<SNqAction>& v = n->Slot(slot.c_str());
				if (index < (int)v.size()) v.insert(v.begin() + index + 1, v[index]);
			});
		}
		if (ImGui::MenuItem("Remove action") && index >= 0)
		{
			xr_string id = m_CtxNode;
			m_Doc->Edit([&](SNqQuest& q)
			{
				SNqNode* n = q.FindNode(id.c_str()); if (!n) return;
				xr_vector<SNqAction>& v = n->Slot(slot.c_str());
				if (index < (int)v.size()) v.erase(v.begin() + index);
			});
			m_Doc->sel_slot.clear();
		}
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("nq_link_ctx"))
	{
		ImGui::TextDisabled("%s.%s -> %s", m_CtxLinkFrom.c_str(), m_CtxLinkPin.c_str(), m_CtxLinkTo.c_str());
		if (ImGui::MenuItem("Remove link"))
		{
			xr_string err;
			m_Doc->Disconnect(m_CtxLinkFrom.c_str(), m_CtxLinkPin.c_str(), m_CtxLinkTo.c_str(), err);
		}
		ImGui::EndPopup();
	}
}

//------------------------------------------------------------------------------
// frame
//------------------------------------------------------------------------------
void NqCanvas::Draw(const ImVec2& size)
{
	m_Origin = ImGui::GetCursorScreenPos();
	m_Size = ImVec2(_max(size.x, 32.f), _max(size.y, 32.f));
	// the view is not part of the file, but panning, framing and quest_view all
	// move it: bound it before anything is projected through it
	m_Doc->view_cx = NqLayout::Sane(m_Doc->view_cx);
	m_Doc->view_cy = NqLayout::Sane(m_Doc->view_cy);

	// deferred framing requests need the rect
	if (m_WantFrameAll)			{ FrameAll(); m_WantFrameAll = false; }
	if (m_WantFrameSel)			{ FrameSelection(); m_WantFrameSel = false; }
	if (!m_WantFrameNode.empty()) { FrameNode(m_WantFrameNode.c_str()); m_WantFrameNode.clear(); }
	// framing picks a zoom of its own; an explicit one was asked for later
	if (m_WantZoom >= 0)		{ m_Doc->zoom_idx = m_WantZoom; m_WantZoom = -1; }

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(m_Origin, Add(m_Origin, m_Size), Col(30, 30, 33));
	dl->PushClipRect(m_Origin, Add(m_Origin, m_Size), true);

	// one invisible button owns the mouse for the whole area
	ImGui::InvisibleButton("##nq_canvas", m_Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	m_Hovered = ImGui::IsItemHovered();

	EnsureReachable();
	BuildGeometry();
	BuildLinks();

	DrawGrid(dl);
	DrawLinks(dl);
	DrawLinking(dl);
	for (u32 i = 0; i < m_Geom.size(); ++i) DrawNode(dl, (int)i);
	DrawMarquee(dl);

	// zoom label
	{
		char lab[32]; xr_sprintf(lab, "%d%%", int(Zoom() * 100.f + 0.5f));
		dl->AddText(ImVec2(m_Origin.x + 6.f, m_Origin.y + m_Size.y - 18.f), Col(200, 200, 200, 160), lab);
	}
	// why the last link drop was refused - a fading line, never a modal
	if (!m_Refused.empty())
	{
		if (ImGui::GetTime() - m_RefusedAt > 2.5) m_Refused.clear();
		else dl->AddText(ImVec2(m_Origin.x + 6.f, m_Origin.y + 6.f), Col(255, 190, 120, 230), m_Refused.c_str());
	}
	dl->PopClipRect();

	HandleInput();
	HandleKeys();
	DrawContextMenus();
}
