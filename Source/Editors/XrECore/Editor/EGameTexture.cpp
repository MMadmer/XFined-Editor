//------------------------------------------------------------------------------
// Textures for content browsed out of the linked game install.
//
// The game's archives are mounted in EditorGameContent's own registry, which the
// editor FS deliberately cannot see - that is what stops a game asset from ever
// shadowing an SDK one. The cost is that a model previewed straight from the
// game references textures the normal loader cannot find, and the preview comes
// out black. CTexture::Load falls back here after its own lookup fails.
//
// Read-only by construction: the only calls into the game install are Find and
// ReadBytes.
//------------------------------------------------------------------------------
#include "stdafx.h"
#pragma hdrstop

#if defined(USE_DX11)

#include "EditorGameContent.h"
#include "RedImageTool/RedImage.hpp"

ID3DBaseTexture* EditorGameContent_LoadTexture(LPCSTR name, u32& mem)
{
	mem = 0;
	if (!name || !name[0] || !HW.pDevice)	return 0;

	// Engine texture names carry no extension and are relative to the textures
	// root, and they use backslashes. The game registry keys everything with
	// FORWARD slashes (see darf_list), so the separator has to be translated or
	// the lookup silently misses - which is exactly what left previews black.
	string_path	rel;
	{
		string_path	clean;
		xr_strcpy	(clean, sizeof(clean), name);
		if (LPSTR ext = strext(clean))
			if (0 == _stricmp(ext, ".dds"))	*ext = 0;
		for (char* p = clean; *p; ++p)
			if (*p == '\\')	*p = '/';
		xr_sprintf	(rel, sizeof(rel), "textures/%s.dds", clean);
	}

	int idx = EditorGameContent::Find(rel);
	if (idx < 0)
	{
		// tolerate a registry that keys on backslashes instead
		for (char* p = rel; *p; ++p)
			if (*p == '/')	*p = '\\';
		idx = EditorGameContent::Find(rel);
	}
	if (idx < 0)	return 0;

	u32	size	= 0;
	u8*	bytes	= EditorGameContent::ReadBytes(idx, size);
	if (!bytes || !size)
	{
		EditorGameContent::FreeBytes(bytes);
		return 0;
	}

	RedImageTool::RedImage img;
	const bool decoded = img.LoadFromMemory(bytes, size);
	EditorGameContent::FreeBytes(bytes);
	if (!decoded)	return 0;

	// one level is enough for a preview, and B8G8R8A8 is what the rest of the
	// editor's D3D11 upload paths speak
	img.ClearMipLevels	();
	img.Convert			(RedImageTool::RedTexturePixelFormat::R8G8B8A8);
	img.SwapRB			();

	const u32 w = u32(img.GetWidth());
	const u32 h = u32(img.GetHeight());
	if (!w || !h)	return 0;

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
	data.pSysMem			= (const void*)*img;
	data.SysMemPitch		= w * sizeof(u32);

	ID3D11Texture2D* tex = 0;
	if (FAILED(HW.pDevice->CreateTexture2D(&desc, &data, &tex)) || !tex)
		return 0;

	mem = w * h * u32(sizeof(u32));
	return tex;
}

#endif	//	USE_DX11
