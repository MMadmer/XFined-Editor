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

	// Grab zones, in screen pixels at 100% DPI. The dot a pin is drawn as follows
	// the zoom and is three pixels wide at 20%, so the zone cannot be the dot: it
	// is the dot plus a fixed slack, floored so that the far zoom levels stay
	// usable and capped so that a pin never swallows its own node. Towards the
	// body the zone is tightened back to roughly the drawn size - the bottom band
	// of a box has to keep starting a node drag, not a link.
	const float kPinGrabPad		= 8.f;
	const float kPinGrabMin		= 12.f;
	const float kPinGrabMax		= 22.f;
	const float kPinGrabInside	= 0.7f;		// share of the radius that reaches inwards
	const float kLinkGrab		= 9.f;		// around a link curve

	const float kPickerRows	= 12.f;		// rows a search popup shows before it scrolls
	const float kPickerMinW	= 320.f;
	const float kPickerMaxW	= 640.f;
	const int	kHistoryLimit	= 64;

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
	IC float Dist2(const ImVec2& a, const ImVec2& b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y); }

	// `out` is pin -> targets, so a pin is free exactly when it carries none
	bool PinFree(const SNqNode& n, LPCSTR pin)
	{
		const xr_vector<xr_string>* t = n.Targets(pin);
		return !t || t->empty();
	}

	// a node can take one more incoming edge when nothing arrives yet; flow.join is
	// the exception - merging several paths is the whole point of it
	bool CanTakeInput(const SNqQuest& q, const SNqNode& n)
	{
		if (NqText::IsTrigger(n.kind.c_str())) return false;
		return n.kind == "flow.join" || 0 == q.InDegree(n.id.c_str());
	}

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

	// Case folding for the search popups. The editor holds UTF-8, so a byte-wise
	// tolower() would fold Latin only and leave "Ждёт" unreachable from "ждёт" -
	// which reads as "the search is broken". Cyrillic is folded by hand:
	// U+0410..U+042F -> U+0430..U+044F and Ё (U+0401) -> ё (U+0451).
	void Fold(LPCSTR s, xr_string& out)
	{
		out.clear();
		if (!s) return;
		for (const u8* p = (const u8*)s; *p; ++p)
		{
			const u8 c = *p;
			if (c >= 'A' && c <= 'Z') { out += char(c + 0x20); continue; }
			if (c == 0xD0 && p[1])
			{
				const u8 d = p[1];
				// А..П stay in the D0 lead, Р..Я cross over into D1
				if (d == 0x81)				{ out += char(0xD1); out += char(0x91); ++p; continue; }
				if (d >= 0x90 && d <= 0x9F)	{ out += char(0xD0); out += char(d + 0x20); ++p; continue; }
				if (d >= 0xA0 && d <= 0xAF)	{ out += char(0xD1); out += char(d - 0x20); ++p; continue; }
			}
			out += char(c);
		}
	}

	// `needle` is already folded; `scratch` is the caller's buffer for the haystack
	bool FilterHit(LPCSTR text, LPCSTR needle, xr_string& scratch)
	{
		Fold(text, scratch);
		return 0 != strstr(scratch.c_str(), needle);
	}

	// A search popup is auto-sized by its filter box, and its list sits in a fixed
	// child that cannot push the window open - so the width has to be asked for
	// here, measured on the widest human title the popup can show.
	void ConstrainPicker(LPCSTR popup, u32 use_mask)
	{
		if (!ImGui::IsPopupOpen(popup)) return;
		xr_vector<const NqCatalog::SKind*> kinds;
		NqCatalog::KindsFor(use_mask, kinds);
		float w = 0.f;
		for (u32 i = 0; i < kinds.size(); ++i)
			w = _max(w, ImGui::CalcTextSize(kinds[i]->title.c_str()).x + ImGui::CalcTextSize(kinds[i]->id.c_str()).x);
		const ImGuiStyle& st = ImGui::GetStyle();
		w += st.WindowPadding.x * 2.f + st.ItemSpacing.x * 3.f + st.ScrollbarSize + ImGui::GetFontSize();
		w = _min(_max(w, kPickerMinW), kPickerMaxW);
		ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.f), ImVec2(FLT_MAX, FLT_MAX));
	}
}

NqCanvas::NqCanvas(NqDoc* doc) : m_Doc(doc)
{
	m_Origin = m_Size = ImVec2(0, 0);
	m_Hovered = false;
	m_ShowMinimap = true;
	m_MinimapHovered = m_MinimapPanning = false;
	m_MinimapMin = m_MinimapMax = ImVec2(0, 0);
	m_MinimapWorldMin = m_MinimapWorldMax = ImVec2(0, 0);
	m_MinimapScale = 1.f;
	m_Panning = m_RmbMoved = false;
	m_MiddlePanning = false;
	m_Dragging = m_DragMoved = false;
	m_Marquee = false;
	m_Linking = m_PendingLink = false;
	m_StatusAt = 0.0;
	m_HotNode = m_HotPin = -1;
	m_ChipDragIndex = -1;
	m_ChipDragging = false;
	m_WantFrameAll = m_WantFrameSel = m_WantRename = m_WantFocusAction = m_OpenAddAction = false;
	m_WantZoom = -1;
	m_ReplayingHistory = false;
	m_HistoryReady = false;
	m_HistoryBatchDepth = 0;
	m_HistoryBatchRemembered = false;
	m_LastWheelHistory = -DBL_MAX;
	m_WheelZooming = false;
	m_ReachRevision = u32(-1);
	m_GeomRevision = m_GeomCatalogGeneration = u32(-1);
	m_LinkRevision = m_LinkCatalogGeneration = u32(-1);
	m_LinkOrigin = m_LinkSize = ImVec2(FLT_MAX, FLT_MAX);
	m_LinkViewX = m_LinkViewY = FLT_MAX;
	m_LinkZoom = -1;
	m_Filter[0] = 0;
	m_FilterSel = 0;
	if (m_Doc->zoom_idx < 0 || m_Doc->zoom_idx >= kZoomCount) m_Doc->zoom_idx = kZoomDefault;
	// a freshly opened document frames itself once
	m_WantFrameAll = (m_Doc->view_cx == 0.f && m_Doc->view_cy == 0.f);
}

int   NqCanvas::ZoomLevels()			{ return kZoomCount; }
float NqCanvas::ZoomOf(int idx)			{ if (idx < 0) idx = 0; if (idx >= kZoomCount) idx = kZoomCount - 1; return kZoom[idx]; }

NqCanvas::SViewState NqCanvas::CurrentView() const
{
	SViewState state;
	state.cx = m_Doc->view_cx;
	state.cy = m_Doc->view_cy;
	state.zoom = m_Doc->zoom_idx;
	state.selection = m_Doc->selection;
	state.slot = m_Doc->sel_slot;
	return state;
}

void NqCanvas::RememberView()
{
	if (!m_WheelZooming) m_LastWheelHistory = -DBL_MAX;
	if (!m_HistoryReady || m_ReplayingHistory || (m_HistoryBatchDepth && m_HistoryBatchRemembered)) return;
	const SViewState state = CurrentView();
	if (!m_BackHistory.empty())
	{
		const SViewState& last = m_BackHistory.back();
		if (last.cx == state.cx && last.cy == state.cy && last.zoom == state.zoom
			&& last.selection == state.selection && last.slot == state.slot)
		{
			if (m_HistoryBatchDepth) m_HistoryBatchRemembered = true;
			return;
		}
	}
	m_BackHistory.push_back(state);
	if ((int)m_BackHistory.size() > kHistoryLimit) m_BackHistory.erase(m_BackHistory.begin());
	m_ForwardHistory.clear();
	if (m_HistoryBatchDepth) m_HistoryBatchRemembered = true;
}

void NqCanvas::ApplyView(const SViewState& state)
{
	m_ReplayingHistory = true;
	m_LastWheelHistory = -DBL_MAX;
	CancelFraming();
	m_Doc->view_cx = NqLayout::Sane(state.cx);
	m_Doc->view_cy = NqLayout::Sane(state.cy);
	m_Doc->zoom_idx = _max(0, _min(state.zoom, kZoomCount - 1));
	m_Doc->selection.clear();
	for (u32 i = 0; i < state.selection.size(); ++i)
		if (m_Doc->quest.FindNode(state.selection[i].c_str())) m_Doc->selection.push_back(state.selection[i]);
	m_Doc->sel_slot = m_Doc->selection.size() == 1 ? state.slot : "";
	m_ReplayingHistory = false;
}

void NqCanvas::BeginNavigationBatch()
{
	if (!m_HistoryBatchDepth) m_HistoryBatchRemembered = false;
	++m_HistoryBatchDepth;
}

void NqCanvas::EndNavigationBatch()
{
	if (m_HistoryBatchDepth > 0) --m_HistoryBatchDepth;
	if (!m_HistoryBatchDepth) m_HistoryBatchRemembered = false;
}

bool NqCanvas::HistoryBack()
{
	if (m_BackHistory.empty()) return false;
	const SViewState current = CurrentView();
	const SViewState target = m_BackHistory.back();
	m_BackHistory.pop_back();
	m_ForwardHistory.push_back(current);
	if ((int)m_ForwardHistory.size() > kHistoryLimit) m_ForwardHistory.erase(m_ForwardHistory.begin());
	ApplyView(target);
	return true;
}

bool NqCanvas::HistoryForward()
{
	if (m_ForwardHistory.empty()) return false;
	const SViewState current = CurrentView();
	const SViewState target = m_ForwardHistory.back();
	m_ForwardHistory.pop_back();
	m_BackHistory.push_back(current);
	if ((int)m_BackHistory.size() > kHistoryLimit) m_BackHistory.erase(m_BackHistory.begin());
	ApplyView(target);
	return true;
}

void NqCanvas::ClearHistory()
{
	m_BackHistory.clear();
	m_ForwardHistory.clear();
	m_LastWheelHistory = -DBL_MAX;
}

bool NqCanvas::IsBookmarked(LPCSTR id) const
{
	return id && m_BookmarkLookup.find(id) != m_BookmarkLookup.end();
}

bool NqCanvas::SetBookmark(LPCSTR id, bool enabled)
{
	if (!id || !id[0]) return false;
	auto found = m_BookmarkLookup.find(id);
	if (found != m_BookmarkLookup.end())
	{
		if (enabled) return true;
		m_BookmarkLookup.erase(found);
		for (u32 i = 0; i < m_Bookmarks.size(); ++i)
		{
			if (m_Bookmarks[i] != id) continue;
			m_Bookmarks.erase(m_Bookmarks.begin() + i);
			break;
		}
		return true;
	}
	if (!m_Doc->quest.FindNode(id)) return false;
	if (enabled)
	{
		m_Bookmarks.push_back(id);
		m_BookmarkLookup.emplace(id, 1);
	}
	return true;
}

bool NqCanvas::ToggleBookmark(LPCSTR id)
{
	return SetBookmark(id, !IsBookmarked(id));
}

void NqCanvas::ClearBookmarks()
{
	m_Bookmarks.clear();
	m_BookmarkLookup.clear();
}

void NqCanvas::PruneTransient()
{
	for (u32 i = 0; i < m_Bookmarks.size(); )
	{
		if (m_Doc->quest.FindNode(m_Bookmarks[i].c_str())) ++i;
		else
		{
			m_BookmarkLookup.erase(m_Bookmarks[i]);
			m_Bookmarks.erase(m_Bookmarks.begin() + i);
		}
	}
}

void NqCanvas::ToggleSelectedBookmarks()
{
	if (m_Doc->selection.empty()) { SetStatus("select nodes to bookmark"); return; }
	bool all = true;
	for (u32 i = 0; i < m_Doc->selection.size(); ++i) all &= IsBookmarked(m_Doc->selection[i].c_str());
	for (u32 i = 0; i < m_Doc->selection.size(); ++i) SetBookmark(m_Doc->selection[i].c_str(), !all);
	SetStatus(all ? "bookmarks removed" : "bookmarked selection");
}

bool NqCanvas::JumpBookmark(int delta)
{
	PruneTransient();
	if (m_Bookmarks.empty()) return false;
	int current = -1;
	if (m_Doc->selection.size() == 1)
		for (u32 i = 0; i < m_Bookmarks.size(); ++i)
			if (m_Bookmarks[i] == m_Doc->selection[0]) { current = (int)i; break; }
	int next = delta < 0 ? (current < 0 ? (int)m_Bookmarks.size() - 1 : current - 1)
					 : (current + 1);
	if (next < 0) next = (int)m_Bookmarks.size() - 1;
	if (next >= (int)m_Bookmarks.size()) next = 0;
	RequestFrameNode(m_Bookmarks[next].c_str());
	return true;
}

bool NqCanvas::GraphBounds(float& min_x, float& min_y, float& max_x, float& max_y) const
{
	if (m_Doc->quest.nodes.empty()) return false;
	min_x = min_y = FLT_MAX;
	max_x = max_y = -FLT_MAX;
	for (u32 i = 0; i < m_Doc->quest.nodes.size(); ++i)
	{
		const SNqNode& node = m_Doc->quest.nodes[i];
		const Fvector2 size = NqLayout::NodeSize(node);
		min_x = _min(min_x, node.pos.x);
		min_y = _min(min_y, node.pos.y);
		max_x = _max(max_x, node.pos.x + size.x);
		max_y = _max(max_y, node.pos.y + size.y);
	}
	return true;
}

bool NqCanvas::ViewportBounds(float& min_x, float& min_y, float& max_x, float& max_y) const
{
	if (!Measured()) return false;
	const float z = Zoom();
	min_x = m_Doc->view_cx - m_Size.x / (2.f * z);
	min_y = m_Doc->view_cy - m_Size.y / (2.f * z);
	max_x = m_Doc->view_cx + m_Size.x / (2.f * z);
	max_y = m_Doc->view_cy + m_Size.y / (2.f * z);
	return true;
}

// an asked-for zoom outranks the zoom a pending frame request would pick, so
// quest_view with both `frame` and `zoom_level` shows the level that was asked
// for (and the answer it already reported)
void NqCanvas::SetZoom(int idx)
{
	if (idx < 0) idx = 0;
	if (idx >= kZoomCount) idx = kZoomCount - 1;
	if (idx != m_Doc->zoom_idx) RememberView();
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
	FrameNode(id);
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
	RememberView();
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
	const float x = NqLayout::Sane(wx), y = NqLayout::Sane(wy);
	if (x != m_Doc->view_cx || y != m_Doc->view_cy) RememberView();
	CancelFraming();
	m_Doc->view_cx = x;
	m_Doc->view_cy = y;
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
	const float cx = NqLayout::Sane((wmin.x + wmax.x) * 0.5f);
	const float cy = NqLayout::Sane((wmin.y + wmax.y) * 0.5f);
	if (cx != m_Doc->view_cx || cy != m_Doc->view_cy || idx != m_Doc->zoom_idx) RememberView();
	m_Doc->zoom_idx = idx;
	m_Doc->view_cx = cx;
	m_Doc->view_cy = cy;
}

void NqCanvas::FrameAll()
{
	const SNqQuest& q = m_Doc->quest;
	if (q.nodes.empty())
	{
		if (m_Doc->view_cx != 0.f || m_Doc->view_cy != 0.f || m_Doc->zoom_idx != kZoomDefault) RememberView();
		m_Doc->view_cx = m_Doc->view_cy = 0.f;
		m_Doc->zoom_idx = kZoomDefault;
		return;
	}
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
	CancelFraming();
	Fvector2 sz = NqLayout::NodeSize(*n);
	const float cx = n->pos.x + sz.x * 0.5f, cy = n->pos.y + sz.y * 0.5f;
	const int zoom = _max(m_Doc->zoom_idx, kZoomDefault);
	const bool same_selection = m_Doc->selection.size() == 1 && m_Doc->selection[0] == id && m_Doc->sel_slot.empty();
	if (cx != m_Doc->view_cx || cy != m_Doc->view_cy || zoom != m_Doc->zoom_idx || !same_selection) RememberView();
	m_Doc->view_cx = cx;
	m_Doc->view_cy = cy;
	m_Doc->zoom_idx = zoom;
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
	const u32 catalog_generation = NqCatalog::Generation();
	if (!m_Dragging && m_GeomRevision == m_Doc->revision && m_GeomCatalogGeneration == catalog_generation
		&& m_Geom.size() == q.nodes.size()) return;
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
		// everything pushed so far is what the kind declares; what the file adds
		// below would be an E007 pin, so automatic wiring must not aim at it
		g.declared = (int)g.pins.size();
		// pins present in the file but unknown to the catalog still get a handle
		for (u32 p = 0; p < n.out.size(); ++p)
		{
			bool have = false;
			for (u32 e = 0; e < g.pins.size(); ++e) if (g.pins[e] == n.out[p].first) { have = true; break; }
			if (!have) g.pins.push_back(n.out[p].first);
		}
	}
	m_GeomRevision = m_Doc->revision;
	m_GeomCatalogGeneration = catalog_generation;
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
	const SNqQuest& q = m_Doc->quest;
	const u32 catalog_generation = NqCatalog::Generation();
	if (!m_Dragging && m_LinkRevision == m_Doc->revision && m_LinkCatalogGeneration == catalog_generation
		&& m_LinkOrigin.x == m_Origin.x && m_LinkOrigin.y == m_Origin.y
		&& m_LinkSize.x == m_Size.x && m_LinkSize.y == m_Size.y
		&& m_LinkViewX == m_Doc->view_cx && m_LinkViewY == m_Doc->view_cy
		&& m_LinkZoom == m_Doc->zoom_idx) return;

	m_Links.clear();
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
	m_LinkRevision = m_Doc->revision;
	m_LinkCatalogGeneration = catalog_generation;
	m_LinkOrigin = m_Origin;
	m_LinkSize = m_Size;
	m_LinkViewX = m_Doc->view_cx;
	m_LinkViewY = m_Doc->view_cy;
	m_LinkZoom = m_Doc->zoom_idx;
}

// the editor bakes the monitor DPI into the font (XrUIManager loads it at
// 16 * dpi), so the font size is the scale factor the raw constants live in
float NqCanvas::UiScale() const
{
	float fs = ImGui::GetFontSize();
	return fs > 1.f ? fs / 16.f : 1.f;
}

float NqCanvas::PinGrab() const
{
	const float s = UiScale();
	const float drawn = _max(3.f, kPinR * Zoom());		// the dot DrawNode paints
	const float r = drawn + kPinGrabPad * s;
	return _min(_max(r, kPinGrabMin * s), kPinGrabMax * s);
}

// `inward` points from the pin into its node (-1 for an output pin on the bottom
// edge, +1 for the input pin on the top one): that half of the zone stays narrow
bool NqCanvas::NearPin(const ImVec2& s, const ImVec2& c, float inward, float& d2) const
{
	ImVec2 d = Sub(s, c);
	if (d.y * inward > 0.f) d.y /= kPinGrabInside;
	d2 = d.x * d.x + d.y * d.y;
	const float r = PinGrab();
	return d2 <= r * r;
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

int NqCanvas::HitNodeNear(const ImVec2& s, int except) const
{
	const float r = PinGrab(), lim = r * r;
	float best = FLT_MAX;
	int hit = -1;
	// topmost first, strict <: the node drawn last wins an exact tie
	for (int i = (int)m_Geom.size() - 1; i >= 0; --i)
	{
		if (i == except) continue;
		const SNodeGeom& g = m_Geom[i];
		ImVec2 a = ToScreen(g.pos), b = ToScreen(Add(g.pos, g.size));
		float dx = s.x < a.x ? a.x - s.x : (s.x > b.x ? s.x - b.x : 0.f);
		float dy = s.y < a.y ? a.y - s.y : (s.y > b.y ? s.y - b.y : 0.f);
		float d = dx * dx + dy * dy;
		if (d <= lim && d < best) { best = d; hit = i; }
	}
	return hit;
}

int NqCanvas::HitPin(int node, const ImVec2& s, float* out_d2) const
{
	if (node < 0) return -1;
	const SNodeGeom& g = m_Geom[node];
	float best = FLT_MAX;
	int hit = -1;
	// pins of one node sit close together at low zoom, so the nearest one wins
	for (u32 p = 0; p < g.pins.size(); ++p)
	{
		float d2;
		if (NearPin(s, ToScreen(OutputPin(g, (int)p)), -1.f, d2) && d2 < best) { best = d2; hit = (int)p; }
	}
	if (hit >= 0 && out_d2) *out_d2 = best;
	return hit;
}

bool NqCanvas::HitAnyPin(const ImVec2& s, int& node, int& pin) const
{
	node = pin = -1;
	float best = FLT_MAX;
	for (int i = (int)m_Geom.size() - 1; i >= 0; --i)
	{
		float d2 = FLT_MAX;
		int p = HitPin(i, s, &d2);
		if (p >= 0 && d2 < best) { best = d2; node = i; pin = p; }
	}
	return pin >= 0;
}

// pin under the cursor, for the hover highlight. While a link is in the air the
// only pin that matters is the input it would land on - the very node EndLink
// will pick, so the highlight and the drop can never disagree.
void NqCanvas::UpdateHot()
{
	m_HotNode = m_HotPin = -1;
	if (!m_Hovered) return;
	const ImVec2 mouse = ImGui::GetMousePos();
	if (m_Linking)
	{
		int from = m_Doc->quest.NodeIndex(m_LinkFrom.c_str());
		int node = HitNode(mouse);
		if (node < 0) node = HitNodeNear(mouse, from);
		if (node >= 0 && node != from && !m_Geom[node].trigger) m_HotNode = node;
		return;
	}
	if (m_Dragging || m_Marquee || m_Panning) return;
	float best = FLT_MAX;
	for (int i = (int)m_Geom.size() - 1; i >= 0; --i)
	{
		const SNodeGeom& g = m_Geom[i];
		float d2 = FLT_MAX;
		int p = HitPin(i, mouse, &d2);
		if (p >= 0 && d2 < best) { best = d2; m_HotNode = i; m_HotPin = p; }
		if (!g.trigger && NearPin(mouse, ToScreen(InputPin(g)), 1.f, d2) && d2 < best) { best = d2; m_HotNode = i; m_HotPin = -1; }
	}
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
	// the curve is one to four pixels thick: half of it plus the same slack a pin gets
	const float r = kLinkGrab * UiScale() + _max(1.f, 2.f * Zoom()) * 0.5f;
	float best = r * r;		// squared pixels
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
		// A Bezier curve stays inside the convex hull of its control points, so
		// this padded control-point bound cannot cull a visible wire.
		const float pad = _max(th, _max(4.f, 7.f * z));
		const float min_x = _min(_min(l.p0.x, l.p1.x), _min(l.p2.x, l.p3.x)) - pad;
		const float min_y = _min(_min(l.p0.y, l.p1.y), _min(l.p2.y, l.p3.y)) - pad;
		const float max_x = _max(_max(l.p0.x, l.p1.x), _max(l.p2.x, l.p3.x)) + pad;
		const float max_y = _max(_max(l.p0.y, l.p1.y), _max(l.p2.y, l.p3.y)) + pad;
		if (max_x < m_Origin.x || max_y < m_Origin.y || min_x > m_Origin.x + m_Size.x || min_y > m_Origin.y + m_Size.y) continue;
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
	// while an action is in the air this strip says whether it would land here, and
	// exactly between which two chips - a drag across nodes is otherwise a guess
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool drop_here = m_ChipDragging
		&& mouse.y >= tl.y && mouse.y <= tl.y + h && mouse.x >= tl.x && mouse.x <= tl.x + w;
	if (drop_here)
		dl->AddRect(ImVec2(tl.x, tl.y), ImVec2(tl.x + w, tl.y + h), Col(120, 200, 255, 140), 3.f * z, 0, 1.5f * z);

	for (int i = 0; i <= count; ++i)
	{
		ImVec2 a(x, tl.y + pad * 0.5f), b(x + chip, tl.y + h - pad * 0.5f);
		bool plus = (i == count);
		bool selected = !plus && m_Doc->sel_slot == prefix + NqUtil::Format("%d", i);
		bool dragging = !plus && m_ChipDragging && m_ChipDragIndex == i && m_ChipDragSlot == slot && m_ChipDragNode == n.id;
		if (drop_here && mouse.x >= a.x - pad && mouse.x <= b.x)
			dl->AddRectFilled(ImVec2(a.x - pad * 0.5f, a.y), ImVec2(a.x - pad * 0.5f + _max(2.f, 2.f * z), b.y),
				Col(120, 200, 255, 230));
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

	// output pins with labels. A pin the cursor can grab grows and shows the halo
	// of its grab zone: the zone is wider than the dot, and nothing else says so.
	float pr = _max(3.f, kPinR * z);
	float halo = PinGrab();
	for (u32 p = 0; p < g.pins.size(); ++p)
	{
		ImVec2 c = ToScreen(OutputPin(g, (int)p));
		bool hot = m_Linking && m_LinkFrom == n.id && m_LinkPin == g.pins[p];
		bool over = m_HotNode == index && m_HotPin == (int)p;
		if (over) dl->AddCircleFilled(c, halo, Col(255, 220, 120, 38), 16);
		float r = over ? pr * 1.4f : pr;
		dl->AddCircleFilled(c, r, (hot || over) ? Col(255, 220, 120) : Col(230, 230, 230), 12);
		dl->AddCircle(c, r, Col(20, 20, 20), 12, 1.f);
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
		bool over = (m_HotNode == index && m_HotPin < 0);
		if (over) dl->AddCircleFilled(c, halo, Col(255, 220, 120, 38), 16);
		float r = over ? pr * 1.4f : pr;
		dl->AddCircleFilled(c, r, over ? Col(255, 220, 120) : Col(200, 200, 200), 12);
		dl->AddCircle(c, r, Col(20, 20, 20), 12, 1.f);
	}
	if (IsBookmarked(n.id.c_str()))
	{
		const float r = _max(3.f, 5.f * z);
		const ImVec2 c(a.x + r + 3.f, a.y + r + 3.f);
		dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), Col(188, 139, 255));
		dl->AddQuad(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), Col(28, 22, 34), 1.f);
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

ImVec2 NqCanvas::MinimapToWorld(const ImVec2& screen) const
{
	if (m_MinimapScale <= 0.f) return ImVec2(m_Doc->view_cx, m_Doc->view_cy);
	const float x = _max(m_MinimapMin.x, _min(screen.x, m_MinimapMax.x));
	const float y = _max(m_MinimapMin.y, _min(screen.y, m_MinimapMax.y));
	return ImVec2(m_MinimapWorldMin.x + (x - m_MinimapMin.x) / m_MinimapScale,
		m_MinimapWorldMin.y + (y - m_MinimapMin.y) / m_MinimapScale);
}

void NqCanvas::DrawMinimap(ImDrawList* dl)
{
	m_MinimapHovered = false;
	if (!m_ShowMinimap || m_Size.x < 260.f || m_Size.y < 180.f || m_Geom.empty()) return;

	float min_x, min_y, max_x, max_y;
	if (!GraphBounds(min_x, min_y, max_x, max_y)) return;
	const float scale = UiScale();
	const ImVec2 box_size(_min(220.f * scale, m_Size.x * 0.35f), _min(140.f * scale, m_Size.y * 0.32f));
	const ImVec2 box_max(m_Origin.x + m_Size.x - 10.f * scale, m_Origin.y + m_Size.y - 10.f * scale);
	const ImVec2 box_min(box_max.x - box_size.x, box_max.y - box_size.y);
	const float pad = 7.f * scale;
	const ImVec2 available(_max(box_size.x - pad * 2.f, 8.f), _max(box_size.y - pad * 2.f, 8.f));

	const float world_pad = NqLayout::kGrid * 2.f;
	m_MinimapWorldMin = ImVec2(min_x - world_pad, min_y - world_pad);
	m_MinimapWorldMax = ImVec2(max_x + world_pad, max_y + world_pad);
	const float world_w = _max(m_MinimapWorldMax.x - m_MinimapWorldMin.x, 1.f);
	const float world_h = _max(m_MinimapWorldMax.y - m_MinimapWorldMin.y, 1.f);
	m_MinimapScale = _min(available.x / world_w, available.y / world_h);
	const ImVec2 fitted(world_w * m_MinimapScale, world_h * m_MinimapScale);
	m_MinimapMin = ImVec2(box_min.x + (box_size.x - fitted.x) * 0.5f, box_min.y + (box_size.y - fitted.y) * 0.5f);
	m_MinimapMax = Add(m_MinimapMin, fitted);

	dl->AddRectFilled(box_min, box_max, Col(20, 20, 24, 224), 5.f * scale);
	dl->AddRect(box_min, box_max, Col(142, 96, 196, 210), 5.f * scale, 0, 1.f * scale);
	auto map_point = [&](const ImVec2& world)
	{
		return ImVec2(m_MinimapMin.x + (world.x - m_MinimapWorldMin.x) * m_MinimapScale,
			m_MinimapMin.y + (world.y - m_MinimapWorldMin.y) * m_MinimapScale);
	};

	for (u32 i = 0; i < m_Doc->quest.nodes.size(); ++i)
	{
		const SNqNode& node = m_Doc->quest.nodes[i];
		const SNodeGeom& geom = m_Geom[i];
		const ImVec2 from = map_point(Add(geom.pos, ImVec2(geom.size.x * 0.5f, geom.size.y * 0.5f)));
		for (u32 p = 0; p < node.out.size(); ++p)
			for (u32 t = 0; t < node.out[p].second.size(); ++t)
			{
				const int target = m_Doc->quest.NodeIndex(node.out[p].second[t].c_str());
				if (target < 0 || target >= (int)m_Geom.size()) continue;
				const SNodeGeom& target_geom = m_Geom[target];
				const ImVec2 to = map_point(Add(target_geom.pos, ImVec2(target_geom.size.x * 0.5f, target_geom.size.y * 0.5f)));
				dl->AddLine(from, to, Col(184, 184, 192, 105), _max(1.f, scale));
			}
	}

	for (u32 i = 0; i < m_Doc->quest.nodes.size(); ++i)
	{
		const SNqNode& node = m_Doc->quest.nodes[i];
		const SNodeGeom& geom = m_Geom[i];
		const ImVec2 a = map_point(geom.pos);
		const ImVec2 b = map_point(Add(geom.pos, geom.size));
		const ImVec2 visible_b(_max(b.x, a.x + 2.f * scale), _max(b.y, a.y + 2.f * scale));
		dl->AddRectFilled(a, visible_b, FamilyColor(node.kind.c_str()), 1.f * scale);
		if (IsSelected(node.id.c_str())) dl->AddRect(a, visible_b, Col(255, 255, 255, 235), 1.f * scale, 0, 1.5f * scale);
		else if (IsBookmarked(node.id.c_str())) dl->AddRect(a, visible_b, Col(188, 139, 255, 235), 1.f * scale, 0, 1.5f * scale);
	}

	const float z = Zoom();
	const ImVec2 view_min(m_Doc->view_cx - m_Size.x / (2.f * z), m_Doc->view_cy - m_Size.y / (2.f * z));
	const ImVec2 view_max(m_Doc->view_cx + m_Size.x / (2.f * z), m_Doc->view_cy + m_Size.y / (2.f * z));
	ImVec2 va = map_point(view_min), vb = map_point(view_max);
	va.x = _max(m_MinimapMin.x, _min(va.x, m_MinimapMax.x));
	va.y = _max(m_MinimapMin.y, _min(va.y, m_MinimapMax.y));
	vb.x = _max(m_MinimapMin.x, _min(vb.x, m_MinimapMax.x));
	vb.y = _max(m_MinimapMin.y, _min(vb.y, m_MinimapMax.y));
	dl->AddRect(va, vb, Col(225, 205, 255, 240), 0.f, 0, _max(1.f, 1.5f * scale));

	m_MinimapHovered = m_Hovered && InRect(ImGui::GetMousePos(), box_min, box_max);
	if (m_MinimapHovered && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
		ImGui::SetTooltip("Quest minimap - click or drag to navigate");
}

//------------------------------------------------------------------------------
// input
//------------------------------------------------------------------------------
void NqCanvas::HandleInput()
{
	ImGuiIO& io = ImGui::GetIO();
	ImVec2 mouse = ImGui::GetMousePos();
	bool hovered = m_Hovered && !m_MinimapHovered;

	if (m_MinimapHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		RememberView();
		m_MinimapPanning = true;
	}
	if (m_MinimapPanning && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		const ImVec2 world = MinimapToWorld(mouse);
		CancelFraming();
		m_Doc->view_cx = NqLayout::Sane(world.x);
		m_Doc->view_cy = NqLayout::Sane(world.y);
	}
	if (m_MinimapPanning && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_MinimapPanning = false;

	// wheel: zoom around the cursor
	if (hovered && io.MouseWheel != 0.f)
	{
		const double now = ImGui::GetTime();
		const bool coalesce = now - m_LastWheelHistory < 0.35;
		const bool replaying = m_ReplayingHistory;
		const int zoom = m_Doc->zoom_idx;
		if (coalesce) m_ReplayingHistory = true;
		m_WheelZooming = true;
		ZoomBy(io.MouseWheel > 0.f ? 1 : -1, &mouse);
		m_WheelZooming = false;
		m_ReplayingHistory = replaying;
		if (m_Doc->zoom_idx != zoom) m_LastWheelHistory = now;
	}

	// right button: pan while dragging, context menu on a click
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { m_Panning = true; m_RmbMoved = false; m_RmbDown = mouse; }
	if (m_Panning && ImGui::IsMouseDown(ImGuiMouseButton_Right))
	{
		ImVec2 d = io.MouseDelta;
		if (d.x != 0.f || d.y != 0.f)
		{
			if (!m_RmbMoved && fabsf(mouse.x - m_RmbDown.x) + fabsf(mouse.y - m_RmbDown.y) > 3.f)
			{
				RememberView();
				m_RmbMoved = true;
			}
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
		if (!m_MiddlePanning && (io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f)) { RememberView(); m_MiddlePanning = true; }
		float z = Zoom();
		m_Doc->view_cx -= io.MouseDelta.x / z; m_Doc->view_cy -= io.MouseDelta.y / z;
	}
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) m_MiddlePanning = false;

	// left button
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		int node = HitNode(mouse);
		int pin = HitPin(node, mouse);
		if (node < 0)
		{
			// pins stick out below the box: the nearest one over every node wins
			int pnode = -1, ppin = -1;
			if (HitAnyPin(mouse, pnode, ppin)) { node = pnode; pin = ppin; }
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

	// chip drag: reorder inside a strip, move between on_enter and on_exit, or hand
	// the action to another node - one drop path for all three, because the strips
	// are the same widget wherever they are drawn
	if (m_ChipDragIndex >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) m_ChipDragging = true;
	if (m_ChipDragIndex >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !m_ChipDragging)
	{
		m_ChipDragIndex = -1; m_ChipDragNode.clear();		// a plain click, already selected on press
	}
	else if (m_ChipDragIndex >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		int node = HitNode(mouse);
		xr_string slot; int idx = 0; bool plus = false;
		if (node >= 0 && HitChip(node, mouse, slot, idx, plus))
		{
			const xr_string to_id = m_Doc->quest.nodes[node].id;
			const xr_string from_id = m_ChipDragNode, from_slot = m_ChipDragSlot;
			const int from = m_ChipDragIndex;
			const bool same_strip = (to_id == from_id && slot == from_slot);
			// dropping an action on its own place is not a move
			if (!same_strip || (!plus && idx != from))
			{
				int landed = 0;
				m_Doc->Edit([&](SNqQuest& q)
				{
					SNqNode* src = q.FindNode(from_id.c_str());
					SNqNode* dst = q.FindNode(to_id.c_str());
					if (!src || !dst) return;
					xr_vector<SNqAction>& sv = src->Slot(from_slot.c_str());
					if (from >= (int)sv.size()) return;
					SNqAction a = sv[from];
					sv.erase(sv.begin() + from);
					// the same vector when the strip did not change, so the index is
					// clamped against what is left after the erase
					xr_vector<SNqAction>& dv = dst->Slot(slot.c_str());
					int at = plus ? (int)dv.size() : idx;
					if (at > (int)dv.size()) at = (int)dv.size();
					if (at < 0) at = 0;
					dv.insert(dv.begin() + at, a);
					landed = at;
				});
				// follow the action so the inspector keeps showing what was dragged
				m_Doc->selection.clear();
				m_Doc->selection.push_back(to_id);
				m_Doc->sel_slot = slot + ":" + NqUtil::Format("%d", landed);
			}
		}
		m_ChipDragIndex = -1; m_ChipDragNode.clear(); m_ChipDragging = false;
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

// the fading line at the top of the canvas, plus the log so a headless run
// (-nodlg) records what the gesture did
void NqCanvas::SetStatus(LPCSTR text)
{
	m_Status = text ? text : "";
	m_StatusAt = ImGui::GetTime();
	if (!m_Status.empty()) Msg("~ [nq] %s", m_Status.c_str());
}

bool NqCanvas::CanLink(LPCSTR from, LPCSTR to, xr_string& why) const
{
	why.clear();
	const SNqNode* t = m_Doc->quest.FindNode(to);
	if (!t)
		why = NqUtil::Format("'%s' does not exist", to);
	else if (0 == strcmp(from, to))
		why = "a node cannot link to itself";
	else if (NqText::IsTrigger(t->kind.c_str()))
		why = NqUtil::Format("'%s' is a trigger - triggers have no input", t->id.c_str());
	return why.empty();
}

void NqCanvas::EndLink()
{
	m_Linking = false;
	m_Status.clear();
	ImVec2 mouse = ImGui::GetMousePos();
	int node = HitNode(mouse);
	xr_string why;
	if (node >= 0)
	{
		// a refused drop used to vanish without a word; say why instead
		if (!CanLink(m_LinkFrom.c_str(), m_Doc->quest.nodes[node].id.c_str(), why))
		{
			m_Status = why;
			m_StatusAt = ImGui::GetTime();
			Msg("~ [nq] link: %s", m_Status.c_str());
			return;
		}
	}
	else
	{
		// a drop that lands beside a box still means that box: its input pin sits
		// on the edge and hitting it exactly is what was too hard. The source node
		// and a neighbour that could not take the edge anyway are not guesses worth
		// making - for them the drop stays "empty space" and offers a new node.
		node = HitNodeNear(mouse, m_Doc->quest.NodeIndex(m_LinkFrom.c_str()));
		if (node >= 0 && !CanLink(m_LinkFrom.c_str(), m_Doc->quest.nodes[node].id.c_str(), why)) node = -1;
	}
	if (node >= 0)
	{
		xr_string err;
		if (!m_Doc->Connect(m_LinkFrom.c_str(), m_LinkPin.c_str(), m_Doc->quest.nodes[node].id.c_str(), err) && !err.empty())
			Msg("! [nq] connect: %s", err.c_str());
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

// Q: the selected node reaches for the nearest node it can still be wired to,
// in either direction (its free output -> another input, or another free output
// -> its own input). Only pins the catalog declares are candidates and the drop
// goes through CanLink, so the edge can never be one the validator refuses on
// the spot. Distances are world units, so the answer does not depend on the zoom.
void NqCanvas::ConnectNearest()
{
	if (m_Doc->selection.size() != 1) { SetStatus("Q wires one node - select exactly one"); return; }
	int si = -1;
	const SNodeGeom* sg = GeomOf(m_Doc->selection[0].c_str(), &si);
	if (!sg) return;
	const SNqQuest& q = m_Doc->quest;
	const SNqNode& sn = q.nodes[si];

	float best = FLT_MAX;
	xr_string from, pin, to;
	// out of the selection into somebody's input
	for (int p = 0; p < sg->declared; ++p)
	{
		if (!PinFree(sn, sg->pins[p].c_str())) continue;
		ImVec2 a = OutputPin(*sg, p);
		for (u32 i = 0; i < q.nodes.size(); ++i)
		{
			if ((int)i == si || !CanTakeInput(q, q.nodes[i])) continue;
			float d = Dist2(a, InputPin(m_Geom[i]));
			if (d < best) { best = d; from = sn.id; pin = sg->pins[p]; to = q.nodes[i].id; }
		}
	}
	// somebody's free output into the selection
	if (CanTakeInput(q, sn))
	{
		ImVec2 b = InputPin(*sg);
		for (u32 i = 0; i < q.nodes.size(); ++i)
		{
			if ((int)i == si) continue;
			const SNqNode& o = q.nodes[i];
			for (int p = 0; p < m_Geom[i].declared; ++p)
			{
				if (!PinFree(o, m_Geom[i].pins[p].c_str())) continue;
				float d = Dist2(OutputPin(m_Geom[i], p), b);
				if (d < best) { best = d; from = o.id; pin = m_Geom[i].pins[p]; to = sn.id; }
			}
		}
	}
	if (from.empty()) { SetStatus("Q found no free pin to connect"); return; }
	xr_string why;
	if (!CanLink(from.c_str(), to.c_str(), why)) { SetStatus(why.c_str()); return; }
	xr_string err;
	if (!m_Doc->Connect(from.c_str(), pin.c_str(), to.c_str(), err))
		SetStatus(err.empty() ? "connect refused" : err.c_str());
	else
		SetStatus(NqUtil::Format("connected %s.%s -> %s", from.c_str(), pin.c_str(), to.c_str()).c_str());
}

void NqCanvas::HandleKeys()
{
	if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
	// Del / Ctrl+A / F2 belong to the canvas only while the canvas is the part
	// of the tab being worked in - the inspector next to it has its own lists
	if (!m_Hovered && !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) return;
	if (ImGui::GetIO().WantTextInput) return;		// a text field owns the keyboard
	// the same keys are bound globally to the scene, and that layer runs from the
	// window proc before this one: told nothing, it copies an empty scene selection
	// and swallows the key-up on its way
	if (UI) UI->BlockShortCuts();
	ImGuiIO& io = ImGui::GetIO();
	// ImGui::IsKeyPressed defaults to repeat=true. These are one-shot commands, and
	// on auto-repeat they run again every repeat tick - which is how holding Ctrl
	// after a paste spawned nodes without end: the key-up of V never reached ImGui
	// (the editor's global shortcut layer had eaten it), so V read as still down
	// and every repeat while Ctrl was held pasted once more.
	const bool once = false;
	if (ImGui::IsKeyPressed(ImGuiKey_Delete, once))					DeleteSelection();
	if (ImGui::IsKeyPressed(ImGuiKey_Home, once))					FrameAll();
	if (ImGui::IsKeyPressed(ImGuiKey_F, once) && !io.KeyCtrl)		FrameSelection();
	if (ImGui::IsKeyPressed(ImGuiKey_F2, once))						m_WantRename = true;
	// IsAnyItemActive on top of the WantTextInput gate above: a popup filter box
	// (and the canvas button held down mid-drag) keeps Q to itself
	if (ImGui::IsKeyPressed(ImGuiKey_Q, once) && !io.KeyCtrl && !io.KeyAlt && !ImGui::IsAnyItemActive()) ConnectNearest();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, once))		SelectAll();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, once))		CopySelection();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, once))		PasteClipboard();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, once))		DuplicateSelection();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, once))		ToggleSelectedBookmarks();
	if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, once))	HistoryBack();
	if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow, once))	HistoryForward();
	if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, once)) m_Doc->Undo();
	if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y, once) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, once)))) m_Doc->Redo();
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, once))
	{
		m_Linking = false; m_Marquee = false;
		// drops the action back where it came from: the move only happens on release
		m_ChipDragIndex = -1; m_ChipDragNode.clear(); m_ChipDragging = false;
	}
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

// the body both search popups share: a full-width filter box over a scrolling
// list of matches, driven from the keyboard alone
const NqCatalog::SKind* NqCanvas::PickKind(LPCSTR hint, u32 use_mask)
{
	const bool appearing = ImGui::IsWindowAppearing();
	if (appearing) { m_FilterSel = 0; ImGui::SetKeyboardFocusHere(); }
	ImGui::SetNextItemWidth(-1.f);
	bool refilter = appearing;
	if (ImGui::InputTextWithHint("##nq_filter", hint, m_Filter, sizeof(m_Filter))) { m_FilterSel = 0; refilter = true; }
	ImGui::Separator();

	xr_vector<const NqCatalog::SKind*> kinds, shown;
	NqCatalog::KindsFor(use_mask, kinds);
	// the author types what the row shows, in any case and either alphabet
	xr_string needle, scratch;
	Fold(NqUtil::Trim(m_Filter).c_str(), needle);
	xr_string last_group;
	int groups = 0;
	for (u32 i = 0; i < kinds.size(); ++i)
	{
		const NqCatalog::SKind& k = *kinds[i];
		if (!needle.empty() && !FilterHit(k.id.c_str(), needle.c_str(), scratch) && !FilterHit(k.title.c_str(), needle.c_str(), scratch)) continue;
		if (k.group != last_group || shown.empty()) { ++groups; last_group = k.group; }
		shown.push_back(kinds[i]);
	}
	if (shown.empty()) { m_FilterSel = 0; ImGui::TextDisabled("no kinds match"); return 0; }

	// a single-line input leaves Up/Down/Enter alone, so the list answers them
	// while the caret stays in the filter box - no mouse in the loop
	int sel = m_FilterSel;
	if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))	++sel;
	if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))		--sel;
	if (sel < 0) sel = (int)shown.size() - 1;
	if (sel >= (int)shown.size()) sel = 0;
	const bool follow = refilter || sel != m_FilterSel;
	m_FilterSel = sel;

	// not on the first frame: the Enter that opened the popup is still pressed
	const NqCatalog::SKind* picked = 0;
	if (!appearing && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) picked = shown[sel];

	const float rows = _min(kPickerRows, float(shown.size()) + float(groups));
	ImGui::BeginChild("##nq_list", ImVec2(0.f, rows * ImGui::GetTextLineHeightWithSpacing()), false);
	xr_string cur_group;
	for (u32 i = 0; i < shown.size(); ++i)
	{
		const NqCatalog::SKind& k = *shown[i];
		if (k.group != cur_group)
		{
			if (i) ImGui::Separator();
			ImGui::TextDisabled("%s", k.group.c_str());
			cur_group = k.group;
		}
		// the id only disambiguates the row, ImGui hides everything after "##"
		xr_string label = k.title + "##" + k.id;
		if (ImGui::Selectable(label.c_str(), (int)i == sel)) picked = shown[i];
		if (ImGui::IsItemHovered() && !k.desc.empty()) ImGui::SetTooltip("%s", k.desc.c_str());
		if (follow && (int)i == sel) ImGui::SetScrollHereY(0.5f);
		// the technical id keeps its old place at the right edge, but only while
		// the title leaves room for it
		const float idw = ImGui::CalcTextSize(k.id.c_str()).x;
		const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - idw;
		if (x > ImGui::CalcTextSize(k.title.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.f)
		{
			ImGui::SameLine(x);
			ImGui::TextDisabled("%s", k.id.c_str());
		}
	}
	ImGui::EndChild();
	return picked;
}

void NqCanvas::DrawAddNodeMenu(LPCSTR popup)
{
	const u32 mask = NqCatalog::useTrigger | NqCatalog::useMain;
	ConstrainPicker(popup, mask);
	if (!ImGui::BeginPopup(popup)) { if (0 == strcmp(popup, "nq_add_node")) m_PendingLink = false; return; }
	if (const NqCatalog::SKind* k = PickKind("search kinds", mask))
	{
		CreateNode(*k, m_MenuWorld, m_PendingLink);
		m_PendingLink = false;
		ImGui::CloseCurrentPopup();
	}
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
		bool all_bookmarked = !m_Doc->selection.empty();
		for (u32 i = 0; i < m_Doc->selection.size(); ++i) all_bookmarked &= IsBookmarked(m_Doc->selection[i].c_str());
		if (ImGui::MenuItem(all_bookmarked ? "Remove bookmarks" : "Bookmark selection", "Ctrl+B")) ToggleSelectedBookmarks();
		if (ImGui::MenuItem("Duplicate", "Ctrl+D"))		DuplicateSelection();
		if (ImGui::MenuItem("Copy", "Ctrl+C"))			CopySelection();
		if (ImGui::MenuItem("Add on_enter action..."))	{ m_ChipDragSlot = "enter"; m_OpenAddAction = true; }
		if (ImGui::MenuItem("Add on_exit action..."))	{ m_ChipDragSlot = "exit"; m_OpenAddAction = true; }
		if (ImGui::MenuItem("Connect to nearest free pin", "Q", false, m_Doc->selection.size() == 1)) ConnectNearest();
		if (ImGui::MenuItem("Disconnect outputs"))
		{
			xr_string id = m_CtxNode;
			m_Doc->Edit([&](SNqQuest& q) { SNqNode* n = q.FindNode(id.c_str()); if (n) n->out.clear(); });
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Delete", "Del"))			DeleteSelection();
		ImGui::EndPopup();
	}

	ConstrainPicker("nq_add_action", NqCatalog::useExtra);
	if (ImGui::BeginPopup("nq_add_action"))
	{
		if (const NqCatalog::SKind* k = PickKind("search actions", NqCatalog::useExtra))
		{
			xr_string id = m_CtxNode, slot = m_ChipDragSlot, kind = k->id;
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
	// framing picks a zoom of its own; an explicit one was asked for later
	if (m_WantZoom >= 0)		{ m_Doc->zoom_idx = m_WantZoom; m_WantZoom = -1; }
	m_HistoryReady = true;
	PruneTransient();

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(m_Origin, Add(m_Origin, m_Size), Col(30, 30, 33));
	dl->PushClipRect(m_Origin, Add(m_Origin, m_Size), true);

	// one invisible button owns the mouse for the whole area
	ImGui::InvisibleButton("##nq_canvas", m_Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	m_Hovered = ImGui::IsItemHovered();

	EnsureReachable();
	BuildGeometry();
	BuildLinks();
	UpdateHot();

	DrawGrid(dl);
	DrawLinks(dl);
	DrawLinking(dl);
	for (u32 i = 0; i < m_Geom.size(); ++i) DrawNode(dl, (int)i);

	// the action being moved rides under the cursor: the strips only say where it
	// would land, and a drag with nothing in hand reads as nothing happening
	if (m_ChipDragging)
	{
		const float z = Zoom();
		const float cw = kChipW * z, ch = NqLayout::kChipStrip * z - kChipPad * z;
		const ImVec2 m = ImGui::GetIO().MousePos;
		const ImVec2 a(m.x - cw * 0.5f, m.y - ch * 0.5f), b(a.x + cw, a.y + ch);
		// red over anything that is not a strip: releasing there cancels the move,
		// and the author should know that before letting go
		xr_string over_slot; int over_idx = 0; bool over_plus = false;
		const int over_node = HitNode(m);
		const bool ok = over_node >= 0 && HitChip(over_node, m, over_slot, over_idx, over_plus);
		dl->AddRectFilled(a, b, ok ? Col(120, 200, 255, 200) : Col(230, 110, 90, 200), 3.f * z);
		dl->AddRect(a, b, Col(20, 20, 20, 180), 3.f * z);
		char label[16];
		xr_sprintf(label, "%d", m_ChipDragIndex + 1);
		ImFont* font = ImGui::GetFont();
		const float fs = kFontBase * z * 0.85f;
		const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
		dl->AddText(font, fs, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), Col(20, 20, 20), label);
	}
	DrawMarquee(dl);
	DrawMinimap(dl);

	// zoom label
	{
		char lab[32]; xr_sprintf(lab, "%d%%", int(Zoom() * 100.f + 0.5f));
		dl->AddText(ImVec2(m_Origin.x + 6.f, m_Origin.y + m_Size.y - 18.f), Col(200, 200, 200, 160), lab);
	}
	// what the last link gesture did, or why it did nothing - a fading line, never a modal
	if (!m_Status.empty())
	{
		if (ImGui::GetTime() - m_StatusAt > 2.5) m_Status.clear();
		else dl->AddText(ImVec2(m_Origin.x + 6.f, m_Origin.y + 6.f), Col(255, 190, 120, 230), m_Status.c_str());
	}
	dl->PopClipRect();

	HandleInput();
	HandleKeys();
	DrawContextMenus();
}
