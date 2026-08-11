#ifndef	dx10BufferUtils_included
#define	dx10BufferUtils_included
#pragma once
#if defined(USE_DX10) || defined(USE_DX11)

namespace dx10BufferUtils
{
// ECORE_API: LevelEditor.exe builds the detail-mesh buffers itself but the
// implementation ships inside XrECore.dll. No-op outside the editor build.
ECORE_API HRESULT	CreateVertexBuffer( ID3DVertexBuffer** ppBuffer, const void* pData, UINT DataSize, bool bImmutable = true);
ECORE_API HRESULT	CreateIndexBuffer( ID3DIndexBuffer** ppBuffer, const void* pData, UINT DataSize, bool bImmutable = true);
ECORE_API HRESULT	CreateConstantBuffer( ID3DBuffer** ppBuffer, UINT DataSize);
ECORE_API void	ConvertVertexDeclaration( const xr_vector<D3DVERTEXELEMENT9> &declIn, xr_vector<D3D_INPUT_ELEMENT_DESC> &declOut);
};

#endif	//	USE_DX10
#endif	//	dx10BufferUtils_included