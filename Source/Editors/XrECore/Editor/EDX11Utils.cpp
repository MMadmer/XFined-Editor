//---------------------------------------------------------------------------
#include "stdafx.h"
#pragma hdrstop

#include "EDX11Utils.h"

#if defined(USE_DX11)

#include "RedImageTool/RedImage.hpp"

//------------------------------------------------------------------------------
// whole-window capture state, see the header for why this has to exist
namespace
{
	U32Vec	g_frame_px;
	u32		g_frame_w		= 0;
	u32		g_frame_h		= 0;
	u32		g_armed_until	= 0;	// GetTickCount() stamp
	u32		g_last_mirror	= 0;

	// wall-clock, not frame counts: the arm window has to survive the seconds
	// between the request that arms it and the follow-up that reads the frame
	const u32	CAPTURE_ARM_MS		= 6000;
	const u32	CAPTURE_PERIOD_MS	= 100;
}

void	DX11ArmFrameCapture()
{
	g_armed_until	= ::GetTickCount() + CAPTURE_ARM_MS;
}

bool	DX11GetFrameCapture(U32Vec& out, u32& w, u32& h)
{
	if (!g_frame_w || !g_frame_h || g_frame_px.empty())	return false;
	out	= g_frame_px;
	w	= g_frame_w;
	h	= g_frame_h;
	return true;
}

void	DX11MirrorBackbuffer()
{
	const u32 now = ::GetTickCount();
	if (now > g_armed_until)						return;
	// a full readback every frame would stall the loop for as long as capture
	// stays armed, and nothing needs that rate
	if (g_last_mirror && (now - g_last_mirror) < CAPTURE_PERIOD_MS)	return;
	if (!HW.m_pSwapChain)							return;

	ID3D11Texture2D* bb = 0;
	if (FAILED(HW.m_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
		return;

	U32Vec	px;
	u32		w = 0, h = 0;
	if (DX11ReadbackBGRA(bb, px, w, h))
	{
		g_frame_px.swap(px);
		g_frame_w		= w;
		g_frame_h		= h;
		g_last_mirror	= now;
	}
	bb->Release();
}

//------------------------------------------------------------------------------
SDX11Target::SDX11Target()
{
	rt = 0; rtv = 0; ds = 0; dsv = 0; srv = 0; w = 0; h = 0;
}

SDX11Target::~SDX11Target()
{
	release();
}

void SDX11Target::release()
{
	_RELEASE(srv);
	_RELEASE(dsv);
	_RELEASE(ds);
	_RELEASE(rtv);
	_RELEASE(rt);
	w = h = 0;
}

bool SDX11Target::create(u32 width, u32 height, bool with_srv)
{
	if (!width || !height || !HW.pDevice)	return false;
	// same size and the SRV requirement already satisfied - keep what we have
	if (rtv && w == width && h == height && (!with_srv || srv))	return true;
	release();

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width				= width;
	desc.Height				= height;
	desc.MipLevels			= 1;
	desc.ArraySize			= 1;
	desc.Format				= DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count	= 1;
	desc.Usage				= D3D11_USAGE_DEFAULT;
	desc.BindFlags			= D3D11_BIND_RENDER_TARGET | (with_srv ? D3D11_BIND_SHADER_RESOURCE : 0);
	if (FAILED(HW.pDevice->CreateTexture2D(&desc, 0, &rt)))				{ release(); return false; }
	if (FAILED(HW.pDevice->CreateRenderTargetView(rt, 0, &rtv)))		{ release(); return false; }
	if (with_srv && FAILED(HW.pDevice->CreateShaderResourceView(rt, 0, &srv)))
																		{ release(); return false; }

	D3D11_TEXTURE2D_DESC dsd = desc;
	dsd.Format		= DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsd.BindFlags	= D3D11_BIND_DEPTH_STENCIL;
	if (FAILED(HW.pDevice->CreateTexture2D(&dsd, 0, &ds)))				{ release(); return false; }
	if (FAILED(HW.pDevice->CreateDepthStencilView(ds, 0, &dsv)))		{ release(); return false; }

	w = width;
	h = height;
	return true;
}

void SDX11Target::bind()
{
	if (!rtv || !HW.pContext)	return;
	HW.pContext->OMSetRenderTargets(1, &rtv, dsv);

	D3D11_VIEWPORT vp = {};
	vp.TopLeftX	= 0.f;
	vp.TopLeftY	= 0.f;
	vp.Width	= float(w);
	vp.Height	= float(h);
	vp.MinDepth	= 0.f;
	vp.MaxDepth	= 1.f;
	HW.pContext->RSSetViewports(1, &vp);
}

void SDX11Target::clear(u32 argb, float depth)
{
	if (!rtv || !HW.pContext)	return;
	// the colour arrives as 0xAARRGGBB, the API wants normalised RGBA
	const float c[4] =
	{
		float((argb >> 16) & 0xff) / 255.f,
		float((argb >>  8) & 0xff) / 255.f,
		float((argb      ) & 0xff) / 255.f,
		float((argb >> 24) & 0xff) / 255.f,
	};
	HW.pContext->ClearRenderTargetView(rtv, c);
	if (dsv)
		HW.pContext->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth, 0);
}

//------------------------------------------------------------------------------
SDX11TargetGuard::SDX11TargetGuard()
{
	m_OldRTV			= 0;
	m_OldDSV			= 0;
	m_OldViewportCount	= 1;
	ZeroMemory(&m_OldViewport, sizeof(m_OldViewport));
	if (!HW.pContext)	return;
	HW.pContext->OMGetRenderTargets(1, &m_OldRTV, &m_OldDSV);
	HW.pContext->RSGetViewports(&m_OldViewportCount, &m_OldViewport);
}

SDX11TargetGuard::~SDX11TargetGuard()
{
	if (HW.pContext)
	{
		HW.pContext->OMSetRenderTargets(1, &m_OldRTV, m_OldDSV);
		if (m_OldViewportCount)
			HW.pContext->RSSetViewports(1, &m_OldViewport);
	}
	// OMGetRenderTargets hands back references - they are ours to drop
	_RELEASE(m_OldDSV);
	_RELEASE(m_OldRTV);
}

//------------------------------------------------------------------------------
bool DX11ReadbackToPixels(ID3D11Texture2D* src, u32 w, u32 h, U32Vec& out, bool flip)
{
	out.clear();
	if (!src || !w || !h || !HW.pDevice || !HW.pContext)	return false;

	D3D11_TEXTURE2D_DESC desc = {};
	src->GetDesc(&desc);
	// a staging copy is the only way to get GPU memory onto the CPU in D3D11
	desc.Usage				= D3D11_USAGE_STAGING;
	desc.BindFlags			= 0;
	desc.CPUAccessFlags		= D3D11_CPU_ACCESS_READ;
	desc.MiscFlags			= 0;

	ID3D11Texture2D* staging = 0;
	if (FAILED(HW.pDevice->CreateTexture2D(&desc, 0, &staging)) || !staging)	return false;
	HW.pContext->CopyResource(staging, src);

	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(HW.pContext->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
	{
		_RELEASE(staging);
		return false;
	}

	out.resize(size_t(w) * size_t(h));
	u32* dst = out.data();
	for (u32 y = 0; y < h; ++y, dst += w)
	{
		const u32 row	= flip ? (h - 1 - y) : y;
		const u8* line	= (const u8*)m.pData + size_t(m.RowPitch) * size_t(row);
		CopyMemory(dst, line, sizeof(u32) * w);
	}

	HW.pContext->Unmap(staging, 0);
	_RELEASE(staging);
	return true;
}

//------------------------------------------------------------------------------
bool DX11ReadbackBGRA(ID3D11Texture2D* src, U32Vec& out, u32& w, u32& h)
{
	out.clear();
	w = h = 0;
	if (!src)	return false;

	D3D11_TEXTURE2D_DESC d = {};
	src->GetDesc(&d);
	// the copy is byte for byte, so only the 32bpp colour layouts make sense
	const bool is_bgra	= (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM) || (d.Format == DXGI_FORMAT_B8G8R8X8_UNORM) ||
						  (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
	const bool is_rgba	= (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) || (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	if (!is_bgra && !is_rgba)	return false;

	if (!DX11ReadbackToPixels(src, d.Width, d.Height, out, false))	return false;

	// swapping R and B is its own inverse, so one branch covers the conversion
	if (is_rgba)
		for (u32& c : out)
			c = (c & 0xff00ff00) | ((c & 0x00ff0000) >> 16) | ((c & 0x000000ff) << 16);

	w = d.Width;
	h = d.Height;
	return true;
}

//------------------------------------------------------------------------------
ID3D11ShaderResourceView* DX11TextureFromPixels(const u32* pixels, u32 w, u32 h)
{
	if (!pixels || !w || !h || !HW.pDevice)	return 0;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width				= w;
	desc.Height				= h;
	desc.MipLevels			= 1;
	desc.ArraySize			= 1;
	desc.Format				= DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count	= 1;
	desc.Usage				= D3D11_USAGE_IMMUTABLE;
	desc.BindFlags			= D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA data = {};
	data.pSysMem		= pixels;
	data.SysMemPitch	= w * sizeof(u32);

	ID3D11Texture2D* tex = 0;
	if (FAILED(HW.pDevice->CreateTexture2D(&desc, &data, &tex)) || !tex)	return 0;

	ID3D11ShaderResourceView* view = 0;
	const HRESULT hr = HW.pDevice->CreateShaderResourceView(tex, 0, &view);
	// the view holds its own reference now
	_RELEASE(tex);
	return SUCCEEDED(hr) ? view : 0;
}

//------------------------------------------------------------------------------
// Same decoder the DX11 texture_load path already runs on, just pointed at an
// arbitrary picture instead of a game .dds.
static ID3D11ShaderResourceView* DX11ViewFromRedImage(RedImageTool::RedImage& img)
{
	// mips are dead weight for a preview tile, and the pixels have to end up in
	// the B8G8R8A8 order DX11TextureFromPixels uploads - the very layout the old
	// D3DFMT_X8R8G8B8 path produced
	img.ClearMipLevels();
	img.Convert	(RedImageTool::RedTexturePixelFormat::R8G8B8A8);
	img.SwapRB	();

	const u32 w = u32(img.GetWidth());
	const u32 h = u32(img.GetHeight());
	if (!w || !h)	return 0;
	return DX11TextureFromPixels((const u32*)*img, w, h);
}

ID3D11ShaderResourceView* DX11TextureFromFile(LPCSTR full_path)
{
	if (!full_path || !full_path[0])	return 0;
	RedImageTool::RedImage img;
	if (!img.LoadFromFile(full_path))	return 0;
	return DX11ViewFromRedImage(img);
}

ID3D11ShaderResourceView* DX11TextureFromMemory(const void* data, u32 size)
{
	if (!data || !size)	return 0;
	RedImageTool::RedImage img;
	// the block is only read; the missing const on the loader is its own problem
	if (!img.LoadFromMemory(const_cast<void*>(data), size))	return 0;
	return DX11ViewFromRedImage(img);
}

#endif	//	USE_DX11
