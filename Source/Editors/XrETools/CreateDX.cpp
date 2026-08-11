// CreateDX.cpp : Defines the entry point for the DLL application.
//

#include	"stdafx.h"
#include	"D3DX_Wrapper.h"

// the in-tree vertex-cache optimizer (Forsyth), optimize_vertex_order.cpp
void _OptimiseVertexCoherencyTriList(WORD* pwList, int iHowManyTris, u32 optimize_mode);
// misc

extern "C"{ 
	ETOOLS_API UINT WINAPI D3DX_GetDriverLevel(LPDIRECT3DDEVICE9 pDevice)
	{
		return D3DXGetDriverLevel(pDevice);
	}
	
	ETOOLS_API HRESULT WINAPI	
		D3DX_GetImageInfoFromFileInMemory(
		LPCVOID					pSrcData, 
		UINT					SrcDataSize, 
		D3DXIMAGE_INFO*			pSrcInfo)
	{
		return D3DXGetImageInfoFromFileInMemory(pSrcData, SrcDataSize, pSrcInfo);
	}
	
	ETOOLS_API HRESULT WINAPI
		D3DX_CreateCubeTextureFromFileInMemoryEx(
		LPDIRECT3DDEVICE9       pDevice,
		LPCVOID                 pSrcData,
		UINT                    SrcDataSize,
		UINT                    Size,
		UINT                    MipLevels,
		DWORD                   Usage,
		D3DFORMAT               Format,
		D3DPOOL                 Pool,
		DWORD                   Filter,
		DWORD                   MipFilter,
		D3DCOLOR                ColorKey,
		D3DXIMAGE_INFO*         pSrcInfo,
		PALETTEENTRY*           pPalette,
		LPDIRECT3DCUBETEXTURE9* ppCubeTexture)
	{
		return D3DXCreateCubeTextureFromFileInMemoryEx(
			pDevice, pSrcData, SrcDataSize, Size, MipLevels, Usage,
			Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette,
			ppCubeTexture);
	}

	ETOOLS_API HRESULT WINAPI
		D3DX_CreateTextureFromFileInMemoryEx(
		LPDIRECT3DDEVICE9         pDevice,
		LPCVOID                   pSrcData,
		UINT                      SrcDataSize,
		UINT                      Width,
		UINT                      Height,
		UINT                      MipLevels,
		DWORD                     Usage,
		D3DFORMAT                 Format,
		D3DPOOL                   Pool,
		DWORD                     Filter,
		DWORD                     MipFilter,
		D3DCOLOR                  ColorKey,
		D3DXIMAGE_INFO*           pSrcInfo,
		PALETTEENTRY*             pPalette,
		LPDIRECT3DTEXTURE9*       ppTexture)
	{
		return D3DXCreateTextureFromFileInMemoryEx(
			pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage,
			Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, 
			ppTexture);
	}

	ETOOLS_API HRESULT WINAPI
		D3DX_CreateTexture(
		LPDIRECT3DDEVICE9         pDevice,
		UINT                      Width,
		UINT                      Height,
		UINT                      MipLevels,
		DWORD                     Usage,
		D3DFORMAT                 Format,
		D3DPOOL                   Pool,
		LPDIRECT3DTEXTURE9*       ppTexture)
	{
		return D3DXCreateTexture( pDevice, Width, Height, MipLevels, Usage, Format, Pool, ppTexture);
	}

	ETOOLS_API HRESULT WINAPI
		D3DX_ComputeNormalMap(
		LPDIRECT3DTEXTURE9 pTexture,
		LPDIRECT3DTEXTURE9 pSrcTexture,
		const PALETTEENTRY *pSrcPalette,
		DWORD Flags,
		DWORD Channel,
		FLOAT Amplitude)	
	{
		return D3DXComputeNormalMap( pTexture, pSrcTexture, pSrcPalette, Flags, Channel, Amplitude);
	}

	ETOOLS_API HRESULT WINAPI
		D3DX_LoadSurfaceFromSurface(
		LPDIRECT3DSURFACE9        pDestSurface,
		CONST PALETTEENTRY*       pDestPalette,
		CONST RECT*               pDestRect,
		LPDIRECT3DSURFACE9        pSrcSurface,
		CONST PALETTEENTRY*       pSrcPalette,
		CONST RECT*               pSrcRect,
		DWORD                     Filter,
		D3DCOLOR                  ColorKey)
	{
		return D3DXLoadSurfaceFromSurface(pDestSurface, pDestPalette, pDestRect, pSrcSurface, pSrcPalette, pSrcRect, Filter, ColorKey);
	}

	ETOOLS_API HRESULT WINAPI
		D3DX_CompileShader(
		LPCSTR                          pSrcData,
		UINT                            SrcDataLen,
		CONST D3DXMACRO*                pDefines,
		LPD3DXINCLUDE                   pInclude,
		LPCSTR                          pFunctionName,
		LPCSTR                          pTarget,
		DWORD                           Flags,
		LPD3DXBUFFER*                   ppShader,
		LPD3DXBUFFER*                   ppErrorMsgs,
		LPD3DXCONSTANTTABLE*            ppConstantTable)
	{
		return D3DXCompileShader(
			pSrcData, SrcDataLen, pDefines, pInclude, pFunctionName,
			pTarget, Flags, ppShader, ppErrorMsgs, ppConstantTable);
	}

	ETOOLS_API HRESULT WINAPI
		D3DX_CompileShaderFromFile(
		LPCSTR                          pSrcFile,
		CONST D3DXMACRO*                pDefines,
		LPD3DXINCLUDE                   pInclude,
		LPCSTR                          pFunctionName,
		LPCSTR                          pTarget,
		DWORD                           Flags,
		LPD3DXBUFFER*                   ppShader,
		LPD3DXBUFFER*                   ppErrorMsgs,
		LPD3DXCONSTANTTABLE*            ppConstantTable)
	{
		return D3DXCompileShaderFromFile(
			pSrcFile, pDefines, pInclude, pFunctionName, pTarget,
			Flags, ppShader, ppErrorMsgs, ppConstantTable);
	}

	ETOOLS_API HRESULT WINAPI
		D3DX_FindShaderComment(
		CONST DWORD*                    pFunction,
		DWORD                           FourCC,
		LPCVOID*                        ppData,
		UINT*                           pSizeInBytes)
	{
		return D3DXFindShaderComment(pFunction, FourCC, ppData, pSizeInBytes);
	}

	ETOOLS_API HRESULT WINAPI
		D3DX_DeclaratorFromFVF(
		DWORD							FVF,
		D3DVERTEXELEMENT9				pDeclarator[MAX_FVF_DECL_SIZE])
	{
		return D3DXDeclaratorFromFVF(FVF,pDeclarator);
	}

	ETOOLS_API UINT WINAPI 
		D3DX_GetDeclVertexSize(
		CONST D3DVERTEXELEMENT9*		pDecl,
		DWORD							Stream)
	{
		return D3DXGetDeclVertexSize(pDecl,Stream);
	}

	ETOOLS_API UINT WINAPI 
		D3DX_GetDeclLength(
		CONST D3DVERTEXELEMENT9 *pDecl)
	{
		return D3DXGetDeclLength(pDecl);
	}

	ETOOLS_API UINT WINAPI
		D3DX_GetFVFVertexSize(DWORD FVF)
	{
		return D3DXGetFVFVertexSize(FVF);
	}

	ETOOLS_API const char*  WINAPI DX_GetErrorDescription(HRESULT hr)
	{
		// dxerr.lib is dead SDK cargo; the system message table covers the
		// COM/Win32 facilities and everything else gets the raw code, which is
		// what actually matters in a log
		static char buf[256];
		DWORD len = ::FormatMessageA(
			FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, DWORD(hr), 0, buf, DWORD(sizeof(buf) - 1), NULL);
		if (len)
		{
			// strip the trailing CRLF FormatMessage insists on
			while (len && (buf[len-1] == '\r' || buf[len-1] == '\n'))
				buf[--len] = 0;
		}
		else
			xr_sprintf(buf, sizeof(buf), "HRESULT 0x%08X", DWORD(hr));
		return buf;
	}
	ETOOLS_API D3DXMATRIX* WINAPI 
		D3DX_MatrixInverse(          
		D3DXMATRIX *pOut,
		FLOAT *pDeterminant,
		CONST D3DXMATRIX *pM)
	{
		return D3DXMatrixInverse(pOut, pDeterminant,pM);
	}

	ETOOLS_API D3DXMATRIX* WINAPI
		D3DX_MatrixTranspose(          
		D3DXMATRIX *pOut,
		CONST D3DXMATRIX *pM)
	{
		return D3DXMatrixTranspose(pOut, pM);
	}

	ETOOLS_API D3DXPLANE* WINAPI
		D3DX_PlaneNormalize(          
		D3DXPLANE *pOut,
		CONST D3DXPLANE *pP)
	{
		return D3DXPlaneNormalize(pOut, pP);
	}

	ETOOLS_API D3DXPLANE* WINAPI
		D3DX_PlaneTransform(          
		D3DXPLANE *pOut,
		CONST D3DXPLANE *pP,
		CONST D3DXMATRIX *pM)
	{
		return D3DXPlaneTransform(pOut, pP, pM);
	}

	// D3DX-compatible contract, local implementation.
	// remap[newFace] = oldFace - that is how every caller in this tree applies
	// it (m_Faces[it] = _source[remap[it]]). The order comes from the in-tree
	// Forsyth optimizer run on a scratch copy; identical triples are matched
	// through per-triple queues, so duplicated faces keep a stable mapping.
	ETOOLS_API HRESULT WINAPI
		D3DX_OptimizeFaces(
		LPCVOID pIndices,
		UINT NumFaces,
		UINT NumVertices,
		BOOL Indices32Bit,
		DWORD * pFaceRemap)
	{
		if (!pIndices || !pFaceRemap)	return E_POINTER;

		// nothing the optimizer can improve; keep the order
		if (NumFaces < 3 || Indices32Bit)
		{
			if (Indices32Bit && NumFaces >= 3)
				Msg("! D3DX_OptimizeFaces: 32bit indices are not optimised, identity order kept");
			for (UINT i=0; i<NumFaces; ++i)	pFaceRemap[i] = i;
			return S_OK;
		}

		const WORD*	src	= (const WORD*)pIndices;
		WORD*		tmp	= xr_alloc<WORD>(NumFaces*3);
		CopyMemory	(tmp, src, sizeof(WORD)*NumFaces*3);
		_OptimiseVertexCoherencyTriList(tmp, int(NumFaces), 2);	// export-time: quality mode

		// map each ordered triple to the queue of original faces carrying it
		typedef u64							face_key;
		typedef xr_map<face_key, xr_deque<DWORD> >	remap_map;
		remap_map	lookup;
		for (UINT f=0; f<NumFaces; ++f)
		{
			const face_key k = (face_key(src[f*3+0])<<32) | (face_key(src[f*3+1])<<16) | face_key(src[f*3+2]);
			lookup[k].push_back(f);
		}
		for (UINT f=0; f<NumFaces; ++f)
		{
			const face_key k = (face_key(tmp[f*3+0])<<32) | (face_key(tmp[f*3+1])<<16) | face_key(tmp[f*3+2]);
			remap_map::iterator it = lookup.find(k);
			VERIFY(it != lookup.end() && !it->second.empty());
			pFaceRemap[f] = it->second.front();
			it->second.pop_front();
		}
		xr_free(tmp);
		return S_OK;
	}

	// remap[oldVertex] = newVertex, vertices ordered by first use in the face
	// list, unreferenced ones appended in their original order - the exact
	// post-TnL fetch order D3DXOptimizeVertices produced, and the exact way
	// ExportSkeleton applies it.
	ETOOLS_API HRESULT WINAPI
		D3DX_OptimizeVertices(
		LPCVOID pIndices,
		UINT NumFaces,
		UINT NumVertices,
		BOOL Indices32Bit,
		DWORD * pVertexRemap)
	{
		if (!pIndices || !pVertexRemap)	return E_POINTER;

		const DWORD	unset = DWORD(-1);
		for (UINT v=0; v<NumVertices; ++v)	pVertexRemap[v] = unset;

		DWORD next = 0;
		if (Indices32Bit)
		{
			const u32* idx = (const u32*)pIndices;
			for (UINT i=0; i<NumFaces*3; ++i)
			{
				VERIFY(idx[i] < NumVertices);
				if (pVertexRemap[idx[i]] == unset)	pVertexRemap[idx[i]] = next++;
			}
		}
		else
		{
			const WORD* idx = (const WORD*)pIndices;
			for (UINT i=0; i<NumFaces*3; ++i)
			{
				VERIFY(idx[i] < NumVertices);
				if (pVertexRemap[idx[i]] == unset)	pVertexRemap[idx[i]] = next++;
			}
		}
		for (UINT v=0; v<NumVertices; ++v)
			if (pVertexRemap[v] == unset)	pVertexRemap[v] = next++;
		VERIFY(next == NumVertices);
		return S_OK;
	}
}
