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

	struct SFormatInfo
	{
		DXGI_FORMAT	fmt;
		LPCSTR		name;
		u32			block;		// bytes per 4x4 block (compressed) or per pixel
		bool		compressed;
		bool		alpha;
		bool		srgb;
	};

	// Only what this editor's decoders can actually produce. Anything else is
	// reported by its numeric id rather than guessed at.
	static const SFormatInfo s_Formats[] =
	{
		{ DXGI_FORMAT_BC1_UNORM,			"BC1 (DXT1)",		8,	true,	false,	false	},
		{ DXGI_FORMAT_BC1_UNORM_SRGB,		"BC1 sRGB",			8,	true,	false,	true	},
		{ DXGI_FORMAT_BC2_UNORM,			"BC2 (DXT3)",		16,	true,	true,	false	},
		{ DXGI_FORMAT_BC2_UNORM_SRGB,		"BC2 sRGB",			16,	true,	true,	true	},
		{ DXGI_FORMAT_BC3_UNORM,			"BC3 (DXT5)",		16,	true,	true,	false	},
		{ DXGI_FORMAT_BC3_UNORM_SRGB,		"BC3 sRGB",			16,	true,	true,	true	},
		{ DXGI_FORMAT_BC4_UNORM,			"BC4 (ATI1)",		8,	true,	false,	false	},
		{ DXGI_FORMAT_BC5_UNORM,			"BC5 (ATI2)",		16,	true,	false,	false	},
		{ DXGI_FORMAT_BC6H_UF16,			"BC6H",				16,	true,	false,	false	},
		{ DXGI_FORMAT_BC7_UNORM,			"BC7",				16,	true,	true,	false	},
		{ DXGI_FORMAT_BC7_UNORM_SRGB,		"BC7 sRGB",			16,	true,	true,	true	},
		{ DXGI_FORMAT_B8G8R8A8_UNORM,		"B8G8R8A8",			4,	false,	true,	false	},
		{ DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,	"B8G8R8A8 sRGB",	4,	false,	true,	true	},
		{ DXGI_FORMAT_R8G8B8A8_UNORM,		"R8G8B8A8",			4,	false,	true,	false	},
		{ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,	"R8G8B8A8 sRGB",	4,	false,	true,	true	},
		{ DXGI_FORMAT_B8G8R8X8_UNORM,		"B8G8R8X8",			4,	false,	false,	false	},
		{ DXGI_FORMAT_R16G16B16A16_FLOAT,	"R16G16B16A16F",	8,	false,	true,	false	},
		{ DXGI_FORMAT_R8_UNORM,				"R8",				1,	false,	false,	false	},
	};

	const SFormatInfo* FindFormat(DXGI_FORMAT f)
	{
		for (u32 i = 0; i < sizeof(s_Formats)/sizeof(s_Formats[0]); ++i)
			if (s_Formats[i].fmt == f)	return &s_Formats[i];
		return 0;
	}

	// Bytes the whole mip chain costs on the GPU. Block formats round every mip
	// up to whole 4x4 blocks, which is why the small mips are not free.
	u32 EstimateBytes(const D3D11_TEXTURE2D_DESC& d)
	{
		const SFormatInfo* fi = FindFormat(d.Format);
		if (!fi)	return 0;

		u32 total = 0;
		for (u32 m = 0; m < d.MipLevels; ++m)
		{
			u32 w = d.Width  >> m;	if (!w) w = 1;
			u32 h = d.Height >> m;	if (!h) h = 1;
			if (fi->compressed)
				total += ((w + 3) / 4) * ((h + 3) / 4) * fi->block;
			else
				total += w * h * fi->block;
		}
		return total * (d.ArraySize ? d.ArraySize : 1);
	}

	void HumanBytes(u32 bytes, char* dst, u32 dst_size)
	{
		if (bytes >= 1024*1024)	sprintf_s(dst, dst_size, "%.2f MB", double(bytes)/(1024.0*1024.0));
		else if (bytes >= 1024)	sprintf_s(dst, dst_size, "%.1f KB", double(bytes)/1024.0);
		else					sprintf_s(dst, dst_size, "%u B", bytes);
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
	m_Tex		= 0;
	m_View		= 0;
	ZeroMemory	(&m_Desc, sizeof(m_Desc));
	m_FileSize	= 0;
	m_Mip		= 0;
	m_Zoom		= 0.f;		// fit
	m_Alpha		= true;
	m_Channels	= 0;
	m_Focus		= false;
}

UIImagePreview::~UIImagePreview()
{
	Release();
}

void UIImagePreview::Release()
{
	_RELEASE	(m_View);
	_RELEASE	(m_Tex);
	ZeroMemory	(&m_Desc, sizeof(m_Desc));
	m_Mip		= 0;
	m_FileSize	= 0;
}

// Takes the decoder's view, keeps the texture behind it, and re-views it per
// mip. See the header for why swapping here is safe against the draw list.
bool UIImagePreview::Adopt(ID3D11ShaderResourceView* srv)
{
	if (!srv)	return false;

	ID3D11Resource* res = 0;
	srv->GetResource(&res);
	srv->Release();
	if (!res)	return false;

	ID3D11Texture2D* t2d = 0;
	res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2d);
	res->Release();
	if (!t2d)	return false;

	m_Tex = t2d;
	m_Tex->GetDesc(&m_Desc);
	return ViewMip(0);
}

bool UIImagePreview::ViewMip(u32 mip)
{
	if (!m_Tex || !HW.pDevice)		return false;
	if (mip >= m_Desc.MipLevels)	mip = m_Desc.MipLevels ? m_Desc.MipLevels - 1 : 0;

	D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
	vd.Format						= m_Desc.Format;
	vd.ViewDimension				= D3D11_SRV_DIMENSION_TEXTURE2D;
	vd.Texture2D.MostDetailedMip	= mip;
	vd.Texture2D.MipLevels			= 1;

	ID3D11ShaderResourceView* v = 0;
	if (FAILED(HW.pDevice->CreateShaderResourceView(m_Tex, &vd, &v)) || !v)	return false;

	_RELEASE(m_View);
	m_View	= v;
	m_Mip	= mip;
	return true;
}

bool UIImagePreview::Show(LPCSTR name, int source, xr_string* err)
{
	if (err) err->clear();
	if (!name || !name[0])	{ if (err) *err = "empty texture name"; return false; }

	if (!Form) Form = xr_new<UIImagePreview>();
	Form->bOpen		= true;
	Form->m_Focus	= true;

	// already showing this one - just bring the window forward
	if (Form->m_View && (Form->m_Name == name))	return true;

	Form->Release	();
	Form->m_Name	= name;
	Form->m_File	= "";
	Form->m_Error	= "";
	Form->m_Zoom	= 0.f;

	ID3D11ShaderResourceView* srv = 0;

	if (2 == source)
	{
		// the game install: the file may only exist inside an archive
		const int idx = EditorGameContent::Find(name);
		u32 sz = 0;
		u8* bytes = (idx >= 0) ? EditorGameContent::ReadBytes(idx, sz) : 0;
		if (bytes && sz)
		{
			srv					= DX11TextureFromMemory(bytes, sz);
			Form->m_File		= "<linked game install>";
			Form->m_FileSize	= sz;
		}
		EditorGameContent::FreeBytes(bytes);
	}
	else if (0 == source)
	{
		string_path abs;
		xr_sprintf	(abs, sizeof(abs), "%s\\%s", EditorProject::Root(), name);
		srv				= DX11TextureFromFile(abs);
		Form->m_File	= abs;
	}
	else
	{
		// the SDK library: a name without an extension, resolved through fs.ltx
		string_path stem;
		xr_strcpy	(stem, sizeof(stem), name);
		if (char* dot = strrchr(stem, '.'))
			if (!strchr(dot, '\\')) *dot = 0;

		for (int i = 0; i < int(sizeof(s_LibProbes)/sizeof(s_LibProbes[0])) && !srv; ++i)
		{
			string_path rel, full;
			xr_sprintf	(rel, sizeof(rel), "%s%s", stem, s_LibProbes[i].ext);
			FS.update_path(full, s_LibProbes[i].alias, rel);
			if (INVALID_FILE_ATTRIBUTES != ::GetFileAttributesA(full))
			{
				srv = DX11TextureFromFile(full);
				if (srv)	Form->m_File = full;
			}
		}
	}

	if (!srv || !Form->Adopt(srv))
	{
		Form->m_Error = "cannot decode this file as an image";
		if (err) *err = Form->m_Error;
		return false;
	}

	// on-disk size, for the two sources that are files
	if (0 == Form->m_FileSize && !Form->m_File.empty() && '<' != Form->m_File[0])
	{
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (::GetFileAttributesExA(Form->m_File.c_str(), GetFileExInfoStandard, &fad))
			Form->m_FileSize = fad.nFileSizeLow;
	}
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
	ImGui::SetNextItemWidth(110.f);
	// 0 stays "fit"; the slider only takes over once it is dragged
	float z = (m_Zoom > 0.f) ? m_Zoom : 1.f;
	if (ImGui::SliderFloat("##zoom", &z, 0.05f, 16.f, "%.2fx", ImGuiSliderFlags_Logarithmic))
		m_Zoom = z;

	ImGui::SameLine();
	ImGui::SetNextItemWidth(70.f);
	// Channel isolation by tint: multiplying the image is what a channel button
	// can do without a shader of its own. Alpha is judged on the checkerboard.
	const char* kChannels[] = { "RGB", "R", "G", "B" };
	ImGui::Combo("##ch", &m_Channels, kChannels, 4);

	ImGui::SameLine();
	ImGui::Checkbox("Alpha", &m_Alpha);

	if (m_Tex && m_Desc.MipLevels > 1)
	{
		ImGui::SameLine();
		ImGui::SetNextItemWidth(130.f);
		int mip = int(m_Mip);
		if (ImGui::SliderInt("##mip", &mip, 0, int(m_Desc.MipLevels) - 1, "mip %d"))
			ViewMip(u32(mip));
	}

	ImGui::SameLine();
	if (ImGui::Button("Unload"))	{ Release(); m_Name.clear(); m_File.clear(); m_Error.clear(); }
}

void UIImagePreview::DrawInfo()
{
	if (!m_Tex)	return;

	const SFormatInfo* fi = FindFormat(m_Desc.Format);
	char fmt[64];
	if (fi)	xr_strcpy(fmt, sizeof(fmt), fi->name);
	else	sprintf_s(fmt, "DXGI format %u", u32(m_Desc.Format));

	char mem[32], disk[32];
	HumanBytes(EstimateBytes(m_Desc), mem, sizeof(mem));
	HumanBytes(m_FileSize, disk, sizeof(disk));

	if (ImGui::BeginTable("##texinfo", 2, ImGuiTableFlags_SizingFixedFit))
	{
		#define ROW(k, ...)	do {											\
			ImGui::TableNextRow(); ImGui::TableNextColumn();				\
			ImGui::TextDisabled(k); ImGui::TableNextColumn();				\
			ImGui::Text(__VA_ARGS__); } while(0)

		ROW("Imported",		"%u x %u",	m_Desc.Width, m_Desc.Height);
		ROW("Displayed",	"%u x %u  (mip %u of %u)", MipW(), MipH(), m_Mip, m_Desc.MipLevels);
		ROW("Format",		"%s",		fmt);
		ROW("Alpha",		"%s",		fi ? (fi->alpha ? "yes" : "no") : "unknown");
		ROW("sRGB",			"%s",		fi ? (fi->srgb ? "yes" : "no") : "unknown");
		if (m_Desc.ArraySize > 1)	ROW("Slices", "%u", m_Desc.ArraySize);
		ROW("GPU memory",	"%s",		mem);
		if (m_FileSize)		ROW("On disk", "%s", disk);
		if (!m_File.empty())ROW("Source",  "%s", m_File.c_str());

		#undef ROW
		ImGui::EndTable();
	}
}

void UIImagePreview::DrawImage()
{
	const ImVec2 canvas_pos		= ImGui::GetCursorScreenPos();
	const ImVec2 canvas_size	= ImGui::GetContentRegionAvail();
	if (canvas_size.x < 8.f || canvas_size.y < 8.f)	return;

	ImDrawList* dl = ImGui::GetWindowDrawList();

	if (!m_View)
	{
		dl->AddRectFilled(canvas_pos,
			ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
			IM_COL32(43, 43, 43, 255));
		ImGui::Dummy(canvas_size);
		return;
	}

	const float w = float(MipW()), h = float(MipH());

	// fit keeps the aspect and never blows a small texture up past 1:1
	float scale = m_Zoom;
	if (scale <= 0.f)
	{
		scale = _min(canvas_size.x / w, canvas_size.y / h);
		if (scale > 1.f) scale = 1.f;
	}

	const ImVec2 draw(w * scale, h * scale);
	const ImVec2 a(canvas_pos.x + _max(0.f, (canvas_size.x - draw.x) * 0.5f),
				   canvas_pos.y + _max(0.f, (canvas_size.y - draw.y) * 0.5f));
	const ImVec2 b(a.x + draw.x, a.y + draw.y);

	if (m_Alpha)	DrawChecker(dl, a, b);
	else			dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));

	static const ImU32 kTint[4] =
	{
		IM_COL32(255,255,255,255),	// RGB
		IM_COL32(255,  0,  0,255),	// R
		IM_COL32(  0,255,  0,255),	// G
		IM_COL32(  0,  0,255,255),	// B
	};
	dl->AddImage(m_View, a, b, ImVec2(0, 0), ImVec2(1, 1), kTint[m_Channels & 3]);
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
	ImGui::SetNextWindowSize(ImVec2(620, 660), ImGuiCond_FirstUseEver);
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
	else if (!m_View)
		ImGui::TextDisabled("no texture - double click one in the content browser");
	else
		DrawInfo();

	ImGui::Separator();
	DrawImage();

	ImGui::End();
}
