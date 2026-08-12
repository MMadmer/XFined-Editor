#include "stdafx.h"

#include "UIImagePreview.h"
#include "../../../XrECore/Editor/EDX11Utils.h"
#include "../../../XrECore/Editor/EditorGameContent.h"
#include "../../../XrECore/Editor/EditorProject.h"

UIImagePreview* UIImagePreview::Form = nullptr;

namespace
{
	// Where a texture of each browser source physically is. The library names a
	// texture without an extension, and the compiled .dds is what the engine
	// actually ships - the uncompressed source is the fallback.
	struct SProbe { LPCSTR alias; LPCSTR ext; };
	static const SProbe s_LibProbes[] =
	{
		{ "$game_textures$",	".dds"	},
		{ "$textures$",			".tga"	},
		{ "$textures$",			".dds"	},
	};

	static const u32	s_CheckerSize	= 8;
	static const ImU32	s_CheckerA		= IM_COL32(56, 56, 56, 255);
	static const ImU32	s_CheckerB		= IM_COL32(80, 80, 80, 255);

	bool SizeOf(ImTextureID tex, u32& w, u32& h)
	{
		w = h = 0;
		if (!tex)	return false;

		ID3D11Resource* res = 0;
		((ID3D11ShaderResourceView*)tex)->GetResource(&res);
		if (!res)	return false;

		ID3D11Texture2D* t2d = 0;
		res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2d);
		res->Release();
		if (!t2d)	return false;

		D3D11_TEXTURE2D_DESC d = {};
		t2d->GetDesc(&d);
		t2d->Release();
		w = d.Width;
		h = d.Height;
		return (0 != w) && (0 != h);
	}

	// the checkerboard an alpha-blended image is judged against
	void DrawChecker(ImDrawList* dl, const ImVec2& a, const ImVec2& b)
	{
		dl->AddRectFilled(a, b, s_CheckerA);
		for (float y = a.y, row = 0; y < b.y; y += s_CheckerSize, ++row)
			for (float x = a.x + (int(row) & 1 ? s_CheckerSize : 0.f); x < b.x; x += s_CheckerSize * 2.f)
				dl->AddRectFilled(ImVec2(x, y),
								  ImVec2(_min(x + s_CheckerSize, b.x), _min(y + s_CheckerSize, b.y)),
								  s_CheckerB);
	}
}

UIImagePreview::UIImagePreview()
{
	m_Tex	= 0;
	m_W		= 0;
	m_H		= 0;
	m_Zoom	= 0.f;		// fit
	m_Alpha	= true;
	m_Focus	= false;
}

UIImagePreview::~UIImagePreview()
{
	Release();
}

void UIImagePreview::Release()
{
	if (m_Tex)	((ID3D11ShaderResourceView*)m_Tex)->Release();
	m_Tex	= 0;
	m_W		= 0;
	m_H		= 0;
}

bool UIImagePreview::Show(LPCSTR name, int source, xr_string* err)
{
	if (err) err->clear();
	if (!name || !name[0])	{ if (err) *err = "empty texture name"; return false; }

	if (!Form) Form = xr_new<UIImagePreview>();
	Form->bOpen		= true;
	Form->m_Focus	= true;

	// already showing this one - just bring the window forward
	if (Form->m_Tex && (Form->m_Name == name))	return true;

	Form->Release	();
	Form->m_Name	= name;
	Form->m_Error	= "";
	Form->m_Zoom	= 0.f;

	ImTextureID tex = 0;

	if (2 == source)
	{
		// the game install: the file may only exist inside an archive
		const int idx = EditorGameContent::Find(name);
		u32 sz = 0;
		u8* bytes = (idx >= 0) ? EditorGameContent::ReadBytes(idx, sz) : 0;
		if (bytes && sz)	tex = DX11TextureFromMemory(bytes, sz);
		EditorGameContent::FreeBytes(bytes);
	}
	else if (0 == source)
	{
		string_path abs;
		xr_sprintf	(abs, sizeof(abs), "%s\\%s", EditorProject::Root(), name);
		tex = DX11TextureFromFile(abs);
	}
	else
	{
		// the SDK library: a name without an extension, resolved through fs.ltx
		string_path stem;
		xr_strcpy	(stem, sizeof(stem), name);
		if (char* dot = strrchr(stem, '.'))
			if (!strchr(dot, '\\')) *dot = 0;

		for (int i = 0; i < int(sizeof(s_LibProbes)/sizeof(s_LibProbes[0])) && !tex; ++i)
		{
			string_path rel, full;
			xr_sprintf	(rel, sizeof(rel), "%s%s", stem, s_LibProbes[i].ext);
			FS.update_path(full, s_LibProbes[i].alias, rel);
			if (INVALID_FILE_ATTRIBUTES != ::GetFileAttributesA(full))
				tex = DX11TextureFromFile(full);
		}
	}

	if (!tex)
	{
		Form->m_Error = "cannot decode this file as an image";
		if (err) *err = Form->m_Error;
		return false;
	}

	Form->m_Tex = tex;
	if (!SizeOf(tex, Form->m_W, Form->m_H))	{ Form->m_W = Form->m_H = 0; }
	return true;
}

void UIImagePreview::Close()
{
	xr_delete(Form);
}

void UIImagePreview::Update()
{
	if (!Form) return;
	Form->Draw();
	if (Form->IsClosed()) Close();
}

void UIImagePreview::DrawToolBar()
{
	if (ImGui::Button("Fit"))		m_Zoom = 0.f;
	ImGui::SameLine();
	if (ImGui::Button("1:1"))		m_Zoom = 1.f;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.f);
	// 0 stays "fit"; the slider only takes over once it is dragged
	float z = (m_Zoom > 0.f) ? m_Zoom : 1.f;
	if (ImGui::SliderFloat("##zoom", &z, 0.05f, 16.f, "%.2fx", ImGuiSliderFlags_Logarithmic))
		m_Zoom = z;
	ImGui::SameLine();
	ImGui::Checkbox("Alpha", &m_Alpha);
	ImGui::SameLine();
	if (ImGui::Button("Unload"))	{ Release(); m_Name.clear(); m_Error.clear(); }
}

void UIImagePreview::DrawImage()
{
	const ImVec2 canvas_pos		= ImGui::GetCursorScreenPos();
	const ImVec2 canvas_size	= ImGui::GetContentRegionAvail();
	if (canvas_size.x < 8.f || canvas_size.y < 8.f)	return;

	ImDrawList* dl = ImGui::GetWindowDrawList();

	if (!m_Tex || !m_W || !m_H)
	{
		dl->AddRectFilled(canvas_pos,
			ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
			IM_COL32(43, 43, 43, 255));
		ImGui::Dummy(canvas_size);
		return;
	}

	// fit keeps the aspect and never blows a small texture up past 1:1
	float scale = m_Zoom;
	if (scale <= 0.f)
	{
		scale = _min(canvas_size.x / float(m_W), canvas_size.y / float(m_H));
		if (scale > 1.f) scale = 1.f;
	}

	const ImVec2 draw(float(m_W) * scale, float(m_H) * scale);
	const ImVec2 a(canvas_pos.x + _max(0.f, (canvas_size.x - draw.x) * 0.5f),
				   canvas_pos.y + _max(0.f, (canvas_size.y - draw.y) * 0.5f));
	const ImVec2 b(a.x + draw.x, a.y + draw.y);

	if (m_Alpha)	DrawChecker(dl, a, b);
	else			dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));

	dl->AddImage(m_Tex, a, b);
	dl->AddRect (a, b, IM_COL32(90, 90, 90, 255));

	// the wheel zooms, like every image viewer
	ImGui::InvisibleButton("##image_canvas", canvas_size);
	if (ImGui::IsItemHovered())
	{
		const float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.f)
		{
			float z = (m_Zoom > 0.f) ? m_Zoom : scale;
			z *= (wheel > 0.f) ? 1.15f : (1.f / 1.15f);
			m_Zoom = (z < 0.05f) ? 0.05f : (z > 16.f ? 16.f : z);
		}
	}
}

void UIImagePreview::Draw()
{
	xr_string title("Texture");
	if (!m_Name.empty())	{ title += ": "; title += m_Name; }
	title += WindowID();

	if (m_Focus)
	{
		ImGui::SetNextWindowFocus();
		m_Focus = false;
	}
	ImGui::SetNextWindowSize(ImVec2(560, 560), ImGuiCond_FirstUseEver);
	// the wheel belongs to the zoom, not to the window's scrolling
	if (!ImGui::Begin(title.c_str(), &bOpen,
					  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::End();
		return;
	}

	DrawToolBar();
	ImGui::Separator();

	if (!m_Error.empty())
		ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s: %s", m_Name.c_str(), m_Error.c_str());
	else if (!m_Tex)
		ImGui::TextDisabled("no texture - double click one in the content browser");
	else
		ImGui::Text("%s  |  %u x %u", m_Name.c_str(), m_W, m_H);

	DrawImage();

	ImGui::End();
}
