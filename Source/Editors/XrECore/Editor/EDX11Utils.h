#pragma once

//------------------------------------------------------------------------------
// Offscreen rendering helpers for D3D11.
//
// D3D9 had a call for every step: CreateRenderTarget, CreateDepthStencilSurface,
// GetRenderTargetData, LockRect. None of them exist in D3D11 - a render target
// is a texture plus a view, and reading one back means copying into a STAGING
// texture and mapping that. Thumbnails, the model preview and screenshots all
// need the same sequence, so it lives here once.
//
// Bonus: the D3D9 rule that forbade a readback inside an open scene is gone,
// which is what the thumbnail queue was built to work around.
//------------------------------------------------------------------------------

#if defined(USE_DX11)

struct ECORE_API SDX11Target
{
	ID3D11Texture2D*		rt;
	ID3D11RenderTargetView*	rtv;
	ID3D11Texture2D*		ds;
	ID3D11DepthStencilView*	dsv;
	// SRV is only created on demand: ImGui needs one, a screenshot does not
	ID3D11ShaderResourceView* srv;
	u32						w, h;

					SDX11Target		();
					~SDX11Target	();

	// (Re)creates the target; a no-op when the size already matches.
	bool			create			(u32 width, u32 height, bool with_srv = false);
	void			release			();
	bool			valid			() const { return !!rtv; }

	// Binds as the only render target with a matching full-size viewport.
	void			bind			();
	void			clear			(u32 argb, float depth = 1.f);
};

// Saves what an offscreen pass overwrites and puts it back on destruction.
// D3D11 has no state blocks, so the pieces are captured by hand - but only the
// ones an offscreen pass actually touches.
struct ECORE_API SDX11TargetGuard
{
	ID3D11RenderTargetView*	m_OldRTV;
	ID3D11DepthStencilView*	m_OldDSV;
	D3D11_VIEWPORT			m_OldViewport;
	UINT					m_OldViewportCount;

					SDX11TargetGuard	();
					~SDX11TargetGuard	();
};

// Copies a render target into system memory as X8R8G8B8 rows.
// `flip` mirrors vertically, which the old screenshot path did implicitly.
ECORE_API bool	DX11ReadbackToPixels(ID3D11Texture2D* src, u32 w, u32 h, U32Vec& out, bool flip);

// Uploads a pixel block into a fresh immutable texture + SRV. Caller owns the
// view and releases it; the texture is released here once the view holds it.
ECORE_API ID3D11ShaderResourceView* DX11TextureFromPixels(const u32* pixels, u32 w, u32 h);

#endif	//	USE_DX11
