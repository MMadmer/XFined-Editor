//---------------------------------------------------------------------------
#include "stdafx.h"
#pragma hdrstop

#include "EDX11Utils.h"

#if defined(USE_DX11)

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

#endif	//	USE_DX11
