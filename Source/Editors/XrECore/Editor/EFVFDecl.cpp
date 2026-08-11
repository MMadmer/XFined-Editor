//------------------------------------------------------------------------------
// Local implementations of the four D3DX9 FVF/declaration helpers the editor
// still calls (Fvisual, D3DUtils, the DX11 resource manager). They are pure
// arithmetic over public D3D9 structures, and being the LAST thing the editor
// needed from d3dx9.lib, giving them a home here is what finally lets the DLL
// dependency go. Signatures mirror d3dx9mesh.h exactly, so the existing call
// sites and the vendored header's declarations keep working unchanged.
//------------------------------------------------------------------------------
#include "stdafx.h"
#pragma hdrstop

#if defined(USE_DX10) || defined(USE_DX11)

namespace
{
	// bytes occupied by texcoord set `i` under the given FVF
	u32 fvf_texcoord_size(DWORD FVF, u32 i)
	{
		// 2-bit field per set: 0 = 2 floats, 1 = 3, 2 = 4, 3 = 1
		switch ((FVF >> (i * 2 + 16)) & 3)
		{
		case D3DFVF_TEXTUREFORMAT2:	return 2 * 4;
		case D3DFVF_TEXTUREFORMAT3:	return 3 * 4;
		case D3DFVF_TEXTUREFORMAT4:	return 4 * 4;
		default:					return 1 * 4;	// D3DFVF_TEXTUREFORMAT1
		}
	}

	u32 decl_type_size(BYTE type)
	{
		switch (type)
		{
		case D3DDECLTYPE_FLOAT1:	return 4;
		case D3DDECLTYPE_FLOAT2:	return 8;
		case D3DDECLTYPE_FLOAT3:	return 12;
		case D3DDECLTYPE_FLOAT4:	return 16;
		case D3DDECLTYPE_D3DCOLOR:	return 4;
		case D3DDECLTYPE_UBYTE4:
		case D3DDECLTYPE_UBYTE4N:	return 4;
		case D3DDECLTYPE_SHORT2:
		case D3DDECLTYPE_SHORT2N:
		case D3DDECLTYPE_USHORT2N:	return 4;
		case D3DDECLTYPE_SHORT4:
		case D3DDECLTYPE_SHORT4N:
		case D3DDECLTYPE_USHORT4N:	return 8;
		case D3DDECLTYPE_UDEC3:
		case D3DDECLTYPE_DEC3N:		return 4;
		case D3DDECLTYPE_FLOAT16_2:	return 4;
		case D3DDECLTYPE_FLOAT16_4:	return 8;
		default:					VERIFY(!"unknown D3DDECLTYPE");	return 0;
		}
	}
}

extern "C" {

UINT WINAPI D3DXGetFVFVertexSize(DWORD FVF)
{
	u32 size = 0;
	// the editor's formats carry a plain or pre-transformed position - the
	// blend-weight variants never appear in this tree
	switch (FVF & D3DFVF_POSITION_MASK)
	{
	case D3DFVF_XYZ:	size += 12;	break;
	case D3DFVF_XYZRHW:	size += 16;	break;
	case D3DFVF_XYZW:	size += 16;	break;
	default:			VERIFY(!"unsupported FVF position type");	break;
	}
	if (FVF & D3DFVF_NORMAL)	size += 12;
	if (FVF & D3DFVF_PSIZE)		size += 4;
	if (FVF & D3DFVF_DIFFUSE)	size += 4;
	if (FVF & D3DFVF_SPECULAR)	size += 4;

	const u32 tex_count = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	for (u32 i = 0; i < tex_count; ++i)
		size += fvf_texcoord_size(FVF, i);
	return size;
}

HRESULT WINAPI D3DXDeclaratorFromFVF(DWORD FVF, D3DVERTEXELEMENT9 pDeclarator[MAX_FVF_DECL_SIZE])
{
	if (!pDeclarator)	return E_POINTER;

	u32		n		= 0;
	WORD	offset	= 0;
	D3DVERTEXELEMENT9	e;
	e.Stream	= 0;
	e.Method	= D3DDECLMETHOD_DEFAULT;

	switch (FVF & D3DFVF_POSITION_MASK)
	{
	case D3DFVF_XYZ:
		e.Offset = offset; e.Type = D3DDECLTYPE_FLOAT3; e.Usage = D3DDECLUSAGE_POSITION;  e.UsageIndex = 0;
		pDeclarator[n++] = e; offset += 12;	break;
	case D3DFVF_XYZRHW:
		e.Offset = offset; e.Type = D3DDECLTYPE_FLOAT4; e.Usage = D3DDECLUSAGE_POSITIONT; e.UsageIndex = 0;
		pDeclarator[n++] = e; offset += 16;	break;
	case D3DFVF_XYZW:
		e.Offset = offset; e.Type = D3DDECLTYPE_FLOAT4; e.Usage = D3DDECLUSAGE_POSITION;  e.UsageIndex = 0;
		pDeclarator[n++] = e; offset += 16;	break;
	default:
		VERIFY(!"unsupported FVF position type");
		return E_FAIL;
	}
	if (FVF & D3DFVF_NORMAL)
	{
		e.Offset = offset; e.Type = D3DDECLTYPE_FLOAT3;   e.Usage = D3DDECLUSAGE_NORMAL; e.UsageIndex = 0;
		pDeclarator[n++] = e; offset += 12;
	}
	if (FVF & D3DFVF_PSIZE)
	{
		e.Offset = offset; e.Type = D3DDECLTYPE_FLOAT1;   e.Usage = D3DDECLUSAGE_PSIZE;  e.UsageIndex = 0;
		pDeclarator[n++] = e; offset += 4;
	}
	if (FVF & D3DFVF_DIFFUSE)
	{
		e.Offset = offset; e.Type = D3DDECLTYPE_D3DCOLOR; e.Usage = D3DDECLUSAGE_COLOR;  e.UsageIndex = 0;
		pDeclarator[n++] = e; offset += 4;
	}
	if (FVF & D3DFVF_SPECULAR)
	{
		e.Offset = offset; e.Type = D3DDECLTYPE_D3DCOLOR; e.Usage = D3DDECLUSAGE_COLOR;  e.UsageIndex = 1;
		pDeclarator[n++] = e; offset += 4;
	}

	const u32 tex_count = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	VERIFY(tex_count <= 8);
	for (u32 i = 0; i < tex_count; ++i)
	{
		const u32 bytes = fvf_texcoord_size(FVF, i);
		e.Offset		= offset;
		e.Type			= BYTE(bytes == 8 ? D3DDECLTYPE_FLOAT2 :
							   bytes == 12 ? D3DDECLTYPE_FLOAT3 :
							   bytes == 16 ? D3DDECLTYPE_FLOAT4 : D3DDECLTYPE_FLOAT1);
		e.Usage			= D3DDECLUSAGE_TEXCOORD;
		e.UsageIndex	= BYTE(i);
		pDeclarator[n++] = e;
		offset += WORD(bytes);
	}

	const D3DVERTEXELEMENT9 end = D3DDECL_END();
	pDeclarator[n] = end;
	return S_OK;
}

UINT WINAPI D3DXGetDeclLength(CONST D3DVERTEXELEMENT9* pDecl)
{
	// element count, end marker excluded - callers in this tree +1 when they
	// want the marker included
	if (!pDecl)	return 0;
	UINT n = 0;
	while (pDecl[n].Stream != 0xFF)
	{
		++n;
		VERIFY(n < MAX_FVF_DECL_SIZE);
	}
	return n;
}

UINT WINAPI D3DXGetDeclVertexSize(CONST D3DVERTEXELEMENT9* pDecl, DWORD Stream)
{
	if (!pDecl)	return 0;
	u32 size = 0;
	for (u32 n = 0; pDecl[n].Stream != 0xFF; ++n)
	{
		VERIFY(n < MAX_FVF_DECL_SIZE);
		if (pDecl[n].Stream != Stream)				continue;
		if (pDecl[n].Type == D3DDECLTYPE_UNUSED)	continue;
		const u32 tail = u32(pDecl[n].Offset) + decl_type_size(pDecl[n].Type);
		if (tail > size)	size = tail;
	}
	return size;
}

}	// extern "C"

#endif	//	USE_DX10 || USE_DX11
