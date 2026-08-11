#include "stdafx.h"
#pragma hdrstop

#include "render.h"
#include "ResourceManager.h"
#include "../../../xrAPI/xrAPI.h"
#include "../../xrEngine/irenderable.h"
#include "../../xrEngine/xr_object.h"
#include "../../xrEngine/CustomHUD.h"
#if defined(USE_DX11)
#	include "../../../xrRender/Private/ShaderResourceTraits.h"
#endif
//---------------------------------------------------------------------------
float ssaDISCARD		= 4.f;
float ssaDONTSORT		= 32.f;

ECORE_API float r_ssaDISCARD;
ECORE_API float	g_fSCREEN;

CRender   			RImplementation;

//---------------------
//---------------------------------------------------------------------------
CRender::CRender	()
{
	val_bInvisible = FALSE;
	::Render = &RImplementation;
	m_skinning					= 0;
}

CRender::~CRender	()
{
	xr_delete		(Target);
}

void					CRender::Initialize				()
{
	PSLibrary.OnCreate			();
}
void					CRender::ShutDown				()
{
	PSLibrary.OnDestroy			();
}

void					CRender::OnDeviceCreate			()
{
	Models						= xr_new<CModelPool>	();
    Models->Logging				(FALSE);
}
void					CRender::OnDeviceDestroy		()
{
	xr_delete					(Models);
}

ref_shader	CRender::getShader	(int id){ return 0; }//VERIFY(id<int(Shaders.size()));	return Shaders[id];	}

BOOL CRender::occ_visible(Fbox&	B)
{
	u32 mask		= 0xff;
	return ViewBase.testAABB(B.data(),mask);
}

BOOL CRender::occ_visible(sPoly& P)
{
	return ViewBase.testPolyInside(P);
}

BOOL CRender::occ_visible(vis_data& P)
{
	return occ_visible(P.box);
}

void CRender::Calculate()
{
	// Transfer to global space to avoid deep pointer access
	g_fSCREEN						=	float(EDevice->dwWidth*EDevice->dwHeight);
	r_ssaDISCARD					=	(ssaDISCARD*ssaDISCARD)/g_fSCREEN;
//	r_ssaLOD_A						=	(ssaLOD_A*ssaLOD_A)/g_fSCREEN;
//	r_ssaLOD_B						=	(ssaLOD_B*ssaLOD_B)/g_fSCREEN;
	lstRenderables.clear_not_free();
	ViewBase.CreateFromMatrix		(EDevice->mFullTransform,FRUSTUM_P_LRTB|FRUSTUM_P_FAR);
	{
		g_SpatialSpace->q_frustum
		(
			lstRenderables,
			ISpatial_DB::O_ORDERED,
			STYPE_RENDERABLE + STYPE_LIGHTSOURCE,
			ViewBase
		);

		// Exact sorting order (front-to-back)
	

		// Determine visibility for dynamic part of scene
		set_Object(0);
		if (g_hud)
		{
			g_hud->Render_First();	// R1 shadows
			g_hud->Render_Last();
		}
		u32 uID_LTRACK = 0xffffffff;
		/*if (phase == PHASE_NORMAL)*/
	/*	{
			uLastLTRACK++;
			if (lstRenderables.size())		uID_LTRACK = uLastLTRACK % lstRenderables.size();

			// update light-vis for current entity / actor
			CObject* O = g_pGameLevel->CurrentViewEntity();
			if (O) {
				CROS_impl* R = (CROS_impl*)O->ROS();
				if (R)		R->update(O);
			}
		}*/
		for (ISpatial* pSpatial : lstRenderables)
		{
			IRenderable* renderable = pSpatial->dcast_Renderable();
			if (!renderable)
				continue; 
			if (!(pSpatial->spatial.type & STYPE_RENDERABLE)) 	continue;

			set_Object(renderable);
			renderable->renderable_Render();
			set_Object(nullptr);
		}
	}
}

#include "igame_persistent.h"
void CRender::Render()
{
	
}

IRender_DetailModel*	CRender::model_CreateDM(IReader* F)
{
	VERIFY				(F);
	CDetail*	D		= xr_new<CDetail> ();
	D->Load				(F);
	return D;
}

IRenderVisual*	CRender::model_CreatePE(LPCSTR name)
{
	PS::CPEDef*	source		= PSLibrary.FindPED	(name);
	return Models->CreatePE	(source);
}

IRenderVisual*			CRender::model_CreateParticles	(LPCSTR name)
{
	PS::CPEDef*	SE		= PSLibrary.FindPED	(name);
	if (SE) return		Models->CreatePE	(SE);
	else{
		PS::CPGDef*	SG	= PSLibrary.FindPGD	(name);
		return			SG?Models->CreatePG	(SG):0;
	}
}

#if defined(USE_DX11)
// The depth-range trick is the same, only the viewport now lives on the context
// and its fields are floats.
static void	rm_SetDepthRange	(float zmin, float zmax)
{
	CRenderTarget* T	= RImplementation.getTarget();
	D3D11_VIEWPORT VP;
	VP.TopLeftX	= 0.f;
	VP.TopLeftY	= 0.f;
	VP.Width	= float(T->get_width());
	VP.Height	= float(T->get_height());
	VP.MinDepth	= zmin;
	VP.MaxDepth	= zmax;
	HW.pContext->RSSetViewports(1, &VP);
}
void	CRender::rmNear		()	{ rm_SetDepthRange(0.f,      0.02f); }
void	CRender::rmFar		()	{ rm_SetDepthRange(0.99999f, 1.f  ); }
void	CRender::rmNormal	()	{ rm_SetDepthRange(0.f,      1.f  ); }
#else
void	CRender::rmNear		()
{
	CRenderTarget* T	=	getTarget	();
	D3DVIEWPORT9 VP		=	{0,0,T->get_width(),T->get_height(),0,0.02f };
	CHK_DX				(HW.pDevice->SetViewport(&VP));
}
void	CRender::rmFar		()
{
	CRenderTarget* T	=	getTarget	();
	D3DVIEWPORT9 VP		=	{0,0,T->get_width(),T->get_height(),0.99999f,1.f };
	CHK_DX				(HW.pDevice->SetViewport(&VP));
}
void	CRender::rmNormal	()
{
	CRenderTarget* T	=	getTarget	();
	D3DVIEWPORT9 VP		= {0,0,T->get_width(),T->get_height(),0,1.f };
	CHK_DX				(HW.pDevice->SetViewport(&VP));
}
#endif

void 	CRender::set_Transform	(Fmatrix* M)
{
	current_matrix.set(*M);
}

void			CRender::add_Visual   		(IRenderVisual* visual)			{ if (val_bInvisible)		return; Models->RenderSingle	(dynamic_cast<dxRender_Visual*>(visual),current_matrix,1.f);}
IRenderVisual*	CRender::model_Create		(LPCSTR name, IReader* data)		{ return Models->Create(name,data);		}
IRenderVisual*	CRender::model_CreateChild	(LPCSTR name, IReader* data)		{ return Models->CreateChild(name,data);}
void 			CRender::model_Delete(IRenderVisual*& V, BOOL bDiscard) { auto v = dynamic_cast<dxRender_Visual*>(V); Models->Delete(v, bDiscard); if (v == nullptr)V = nullptr; }
IRenderVisual*	CRender::model_Duplicate	(IRenderVisual* V)					{ return Models->Instance_Duplicate(dynamic_cast<dxRender_Visual*>(V));	}
void 			CRender::model_Render		(IRenderVisual* m_pVisual, const Fmatrix& mTransform, int priority, bool strictB2F, float m_fLOD){Models->Render(dynamic_cast<dxRender_Visual*>(m_pVisual), mTransform, priority, strictB2F, m_fLOD);}
void 			CRender::model_RenderSingle	(IRenderVisual* m_pVisual, const Fmatrix& mTransform, float m_fLOD){Models->RenderSingle(dynamic_cast<dxRender_Visual*>(m_pVisual), mTransform, m_fLOD);}

//#pragma comment(lib,"d3dx_r1")
HRESULT	CRender::CompileShader			(
		LPCSTR                          pSrcData,
		UINT                            SrcDataLen,
		void*							_pDefines,
		void*							_pInclude,
		LPCSTR                          pFunctionName,
		LPCSTR                          pTarget,
		DWORD                           Flags,
		void*							_ppShader,
		void*							_ppErrorMsgs,
		void*							_ppConstantTable)
{
        CONST D3DXMACRO*                pDefines		= (CONST D3DXMACRO*)	_pDefines;
        LPD3DXINCLUDE                   pInclude		= (LPD3DXINCLUDE)		_pInclude;
        LPD3DXBUFFER*                   ppShader		= (LPD3DXBUFFER*)		_ppShader;
        LPD3DXBUFFER*                   ppErrorMsgs		= (LPD3DXBUFFER*)		_ppErrorMsgs;
        LPD3DXCONSTANTTABLE*            ppConstantTable	= (LPD3DXCONSTANTTABLE*)_ppConstantTable;
		return D3DXCompileShader		(pSrcData,SrcDataLen,pDefines,pInclude,pFunctionName,pTarget,Flags,ppShader,ppErrorMsgs,ppConstantTable);
}
HRESULT	CRender::shader_compile			(
		LPCSTR							name,
		LPCSTR                          pSrcData,
		UINT                            SrcDataLen,
		void*							_pDefines,
		void*							_pInclude,
		LPCSTR                          pFunctionName,
		LPCSTR                          pTarget,
		DWORD                           Flags,
		void*							_ppShader,
		void*							_ppErrorMsgs,
		void*							_ppConstantTable)
{
	D3DXMACRO						defines			[128];
	int								def_it			= 0;
	CONST D3DXMACRO*                pDefines		= (CONST D3DXMACRO*)	_pDefines;
	if (pDefines)	{
		// transfer existing defines
		for (;;def_it++)	{
			if (0==pDefines[def_it].Name)	break;
			defines[def_it]			= pDefines[def_it];
		}
	}
	// options
	if (m_skinning<0)		{
		defines[def_it].Name		=	"SKIN_NONE";
		defines[def_it].Definition	=	"1";
		def_it						++;
	}
	if (0==m_skinning)		{
		defines[def_it].Name		=	"SKIN_0";
		defines[def_it].Definition	=	"1";
		def_it						++;
	}
	if (1==m_skinning)		{
		defines[def_it].Name		=	"SKIN_1";
		defines[def_it].Definition	=	"1";
		def_it						++;
	}
	if (2==m_skinning)		{
		defines[def_it].Name		=	"SKIN_2";
		defines[def_it].Definition	=	"1";
		def_it						++;
	}
	if (3 == m_skinning) {
		defines[def_it].Name = "SKIN_3";
		defines[def_it].Definition = "1";
		def_it++;
	}
	if (4 == m_skinning) {
		defines[def_it].Name = "SKIN_4";
		defines[def_it].Definition = "1";
		def_it++;
	}
	// finish
	defines[def_it].Name			=	0;
	defines[def_it].Definition		=	0;
	def_it							++;

	LPD3DXINCLUDE                   pInclude		= (LPD3DXINCLUDE)		_pInclude;
	LPD3DXBUFFER*                   ppShader		= (LPD3DXBUFFER*)		_ppShader;
	LPD3DXBUFFER*                   ppErrorMsgs		= (LPD3DXBUFFER*)		_ppErrorMsgs;
	LPD3DXCONSTANTTABLE*            ppConstantTable	= (LPD3DXCONSTANTTABLE*)_ppConstantTable;
//.	return D3DXCompileShader		(pSrcData,SrcDataLen,defines,pInclude,pFunctionName,pTarget,Flags,ppShader,ppErrorMsgs,ppConstantTable);
#ifdef D3DXSHADER_USE_LEGACY_D3DX9_31_DLL //	December 2006 and later
	HRESULT		_result	= D3DXCompileShader(pSrcData,SrcDataLen,defines,pInclude,pFunctionName,pTarget,Flags|D3DXSHADER_USE_LEGACY_D3DX9_31_DLL,ppShader,ppErrorMsgs,ppConstantTable);
#else
	HRESULT		_result	= D3DXCompileShader(pSrcData,SrcDataLen,defines,pInclude,pFunctionName,pTarget,Flags,ppShader,ppErrorMsgs,ppConstantTable);
#endif
	return _result;
}

#if defined(USE_DX11)
//---------------------------------------------------------------------------
// D3D11 shader compilation.
//
// Same shape as the R4 renderer (XrRender/R4_PC/r4.cpp), minus everything the
// editor has no use for: no r4 quality options, no bytecode disk cache and no
// disassembly dump. Two dozen editor shaders compile in well under a second,
// so paying for a cache (and inventing a cache directory) buys nothing.
//---------------------------------------------------------------------------

// _CreateVS/_CreatePS/_CreateGS never hand an include handler down, so the
// resolver lives here. Rule matches the game renderer: try the renderer's own
// shader subfolder first, then the shared root, both inside $game_shaders$.
class editor_includer : public ID3DInclude
{
public:
	HRESULT __stdcall	Open	(D3D10_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes)
	{
		string_path			pname;
		strconcat			(sizeof(pname), pname, ::Render->getShaderPath(), pFileName);
		IReader*	R		= FS.r_open("$game_shaders$", pname);
		if (!R)
		{
			// shared header - sits outside the per-renderer subfolder
			R				= FS.r_open("$game_shaders$", pFileName);
			if (!R)			return E_FAIL;
		}

		// duplicate and zero-terminate: the compiler wants a C string
		u32			size	= R->length();
		u8*			data	= xr_alloc<u8>(size + 1);
		CopyMemory			(data, R->pointer(), size);
		data[size]			= 0;
		FS.r_close			(R);

		*ppData				= data;
		*pBytes				= size;
		return				S_OK;
	}
	HRESULT __stdcall	Close	(LPCVOID pData)
	{
		xr_free				(pData);
		return				S_OK;
	}
};

// hull/domain/compute: the object and the constant destination both come from
// ShaderTypeTraits, so one body covers all three
template <typename T>
static HRESULT	create_shader	(
	LPCSTR const	pTarget,
	DWORD const*	buffer,
	u32 const		buffer_size,
	LPCSTR const	file_name,
	T*&				result)
{
	result->sh					= ShaderTypeTraits<T>::CreateHWShader(buffer, buffer_size);

	ID3DShaderReflection* pReflection = 0;
	HRESULT const _hr			= D3DReflect(buffer, buffer_size, IID_ID3D11ShaderReflection, (void**)&pReflection);
	if (SUCCEEDED(_hr) && pReflection)
	{
		result->constants.parse	(pReflection, ShaderTypeTraits<T>::GetShaderDest());
		_RELEASE				(pReflection);
	}
	else
	{
		Msg						("! D3DReflect %s hr == 0x%08x", file_name, _hr);
	}

	return						_hr;
}

// dispatch on the profile prefix - CResourceManager passes the shader object
// back as void* and only the target string says what it really is
static HRESULT	create_shader	(
	LPCSTR const	pTarget,
	DWORD const*	buffer,
	u32 const		buffer_size,
	LPCSTR const	file_name,
	void*&			result)
{
	HRESULT		_result	= E_FAIL;

	if ('p' == pTarget[0])
	{
		SPS* sps_result		= (SPS*)result;
		_result				= HW.pDevice->CreatePixelShader(buffer, buffer_size, 0, &sps_result->ps);
		if (!SUCCEEDED(_result))
		{
			Log				("! PS: ", file_name);
			Msg				("! CreatePixelShader hr == 0x%08x", _result);
			return			E_FAIL;
		}

		ID3DShaderReflection* pReflection = 0;
		_result				= D3DReflect(buffer, buffer_size, IID_ID3D11ShaderReflection, (void**)&pReflection);
		if (SUCCEEDED(_result) && pReflection)
		{
			sps_result->constants.parse(pReflection, RC_dest_pixel);
			_RELEASE		(pReflection);
		}
		else
		{
			Log				("! PS: ", file_name);
			Msg				("! D3DReflect hr == 0x%08x", _result);
		}
	}
	else if ('v' == pTarget[0])
	{
		SVS* svs_result		= (SVS*)result;
		_result				= HW.pDevice->CreateVertexShader(buffer, buffer_size, 0, &svs_result->vs);
		if (!SUCCEEDED(_result))
		{
			Log				("! VS: ", file_name);
			Msg				("! CreateVertexShader hr == 0x%08x", _result);
			return			E_FAIL;
		}

		ID3DShaderReflection* pReflection = 0;
		_result				= D3DReflect(buffer, buffer_size, IID_ID3D11ShaderReflection, (void**)&pReflection);
		if (SUCCEEDED(_result) && pReflection)
		{
			// D3D11 builds the input layout from (declaration, VS signature),
			// so the signature blob has to be kept next to the shader
			ID3DBlob*	pSignatureBlob = 0;
			CHK_DX			(D3DGetInputSignatureBlob(buffer, buffer_size, &pSignatureBlob));
			VERIFY			(pSignatureBlob);

			svs_result->signature = EDevice->Resources->_CreateInputSignature(pSignatureBlob);

			_RELEASE		(pSignatureBlob);

			svs_result->constants.parse(pReflection, RC_dest_vertex);
			_RELEASE		(pReflection);
		}
		else
		{
			Log				("! VS: ", file_name);
			Msg				("! D3DReflect hr == 0x%08x", _result);
		}
	}
	else if ('g' == pTarget[0])
	{
		SGS* sgs_result		= (SGS*)result;
		_result				= HW.pDevice->CreateGeometryShader(buffer, buffer_size, 0, &sgs_result->gs);
		if (!SUCCEEDED(_result))
		{
			Log				("! GS: ", file_name);
			Msg				("! CreateGeometryShader hr == 0x%08x", _result);
			return			E_FAIL;
		}

		ID3DShaderReflection* pReflection = 0;
		_result				= D3DReflect(buffer, buffer_size, IID_ID3D11ShaderReflection, (void**)&pReflection);
		if (SUCCEEDED(_result) && pReflection)
		{
			sgs_result->constants.parse(pReflection, RC_dest_geometry);
			_RELEASE		(pReflection);
		}
		else
		{
			Log				("! GS: ", file_name);
			Msg				("! D3DReflect hr == 0x%08x", _result);
		}
	}
	else if ('c' == pTarget[0])
	{
		_result				= create_shader(pTarget, buffer, buffer_size, file_name, (SCS*&)result);
	}
	else if ('h' == pTarget[0])
	{
		_result				= create_shader(pTarget, buffer, buffer_size, file_name, (SHS*&)result);
	}
	else if ('d' == pTarget[0])
	{
		_result				= create_shader(pTarget, buffer, buffer_size, file_name, (SDS*&)result);
	}
	else
	{
		NODEFAULT;
	}

	return					_result;
}

HRESULT	CRender::shader_compile			(
		LPCSTR							name,
		DWORD const*                    pSrcData,
		UINT                            SrcDataLen,
		LPCSTR                          pFunctionName,
		LPCSTR                          pTarget,
		DWORD                           Flags,
		void*&							result)
{
	D3D_SHADER_MACRO				defines			[128];
	int								def_it			= 0;

	// Skinning - same switch the D3D9 overload above uses. Without it
	// shaders\editor\skin_main.h emits no main() at all and the compile dies
	// with "entrypoint not found".
	if (m_skinning<0)		{
		defines[def_it].Name		=	"SKIN_NONE";
		defines[def_it].Definition	=	"1";
		def_it						++;
	}
	if (0==m_skinning)		{
		defines[def_it].Name		=	"SKIN_0";
		defines[def_it].Definition	=	"1";
		def_it						++;
	}
	if (1==m_skinning)		{
		defines[def_it].Name		=	"SKIN_1";
		defines[def_it].Definition	=	"1";
		def_it						++;
	}
	if (2==m_skinning)		{
		defines[def_it].Name		=	"SKIN_2";
		defines[def_it].Definition	=	"1";
		def_it						++;
	}
	if (3==m_skinning)		{
		defines[def_it].Name		=	"SKIN_3";
		defines[def_it].Definition	=	"1";
		def_it						++;
	}
	if (4==m_skinning)		{
		defines[def_it].Name		=	"SKIN_4";
		defines[def_it].Definition	=	"1";
		def_it						++;
	}

	// finish
	defines[def_it].Name			=	0;
	defines[def_it].Definition		=	0;
	def_it							++;

	// The resource manager still asks for D3D9 profiles ("vs_2_0"/"ps_2_0").
	// Everything under gamedata\shaders\editor is SM4+ with a "main" entry, so
	// that is the signal to remap onto what the device actually supports.
	if (0==xr_strcmp(pFunctionName,"main"))
	{
		if ('v'==pTarget[0])
		{
			if		(HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0)	pTarget = "vs_5_0";
			else if	(HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1)	pTarget = "vs_4_1";
			else												pTarget = "vs_4_0";
		}
		else if ('p'==pTarget[0])
		{
			if		(HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0)	pTarget = "ps_5_0";
			else if	(HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1)	pTarget = "ps_4_1";
			else												pTarget = "ps_4_0";
		}
		else if ('g'==pTarget[0])
		{
			if		(HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0)	pTarget = "gs_5_0";
			else if	(HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1)	pTarget = "gs_4_1";
			else												pTarget = "gs_4_0";
		}
		else if ('c'==pTarget[0])
		{
			if		(HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0)	pTarget = "cs_5_0";
		}
	}

	editor_includer				Includer;
	ID3DBlob*					pShaderBuf	= NULL;
	ID3DBlob*					pErrorBuf	= NULL;

	HRESULT		_result			=
		D3DCompile(
			pSrcData,
			SrcDataLen,
			"",				//	NULL trips an old NVPerfHUD bug
			defines, &Includer, pFunctionName,
			pTarget,
			Flags, 0,
			&pShaderBuf,
			&pErrorBuf
		);

	if (SUCCEEDED(_result))
	{
		_result					= create_shader(pTarget, (DWORD*)pShaderBuf->GetBufferPointer(), (u32)pShaderBuf->GetBufferSize(), name, result);
	}
	else
	{
		Log						("! shader compilation failed: ", name);
		if (pErrorBuf)
			Log					("! error: ", (LPCSTR)pErrorBuf->GetBufferPointer());
		else
			Msg					("! Can't compile shader hr == 0x%08x", _result);
	}

	_RELEASE					(pShaderBuf);
	_RELEASE					(pErrorBuf);

	return						_result;
}
#else	//	USE_DX11
HRESULT	CRender::shader_compile			(
		LPCSTR							name,
		DWORD const*                    pSrcData,
		UINT                            SrcDataLen,
		LPCSTR                          pFunctionName,
		LPCSTR                          pTarget,
		DWORD                           Flags,
		void*&							result)
{
	// D3D9 goes through the D3DX overload above; nothing calls this one
	return E_FAIL;
}
#endif	//	USE_DX11

void					CRender::reset_begin			()
{
	xr_delete			(Target);
}
void					CRender::reset_end				()
{
	Target			=	xr_new<CRenderTarget>			();
}

void CRender::set_HUD(BOOL V)
{
}

BOOL CRender::get_HUD()
{
	return 0;
}

void CRender::set_Invisible(BOOL V)
{
	val_bInvisible = V;
}


DWORD CRender::get_dx_level()
{
	return 90;
}

void CRender::create()
{

}
void CRender::destroy()
{

}

void CRender::level_Load(IReader*)
{

}
void CRender::level_Unload()
{

}

// IDirect3DBaseTexture9*	texture_load			(LPCSTR	fname, u32& msize)					= 0;



//	 ref_shader				getShader				(int id)									= 0;
IRender_Sector* CRender::getSector(int id)
{
	return nullptr;
 }
IRenderVisual* CRender::getVisual(int id)
{
	return nullptr;
}
IRender_Sector* CRender::detectSector(const Fvector& P)
{
	return nullptr;
}

void CRender::flush() {}
void CRender::set_Object(IRenderable* O) {}
void CRender::add_Occluder(Fbox2& bb_screenspace) {}
void CRender::add_Geometry(IRenderVisual* V) {}
class RenderObjectSpecific :public IRender_ObjectSpecific
{
public:
	RenderObjectSpecific() {}
	virtual ~RenderObjectSpecific() {}

	virtual	void						force_mode(u32 mode)
	{}
	virtual float						get_luminocity() { return 1; }
	virtual float						get_luminocity_hemi() { return 1; }
	virtual float* get_luminocity_hemi_cube() {
		static float test[8] = {};
		return test;
	}

};
 IRender_ObjectSpecific* CRender::ros_create(IRenderable* parent) { return xr_new< RenderObjectSpecific>(); }
 void CRender::ros_destroy(IRender_ObjectSpecific*& a) { xr_delete(a); }
 class RLight : public IRender_Light
 {
 public:
 public:
	 virtual void set_type(LT type) {}
	 virtual void set_active(bool) {}
	 virtual bool get_active() { return false; }
	 virtual void set_shadow(bool) {}
	 virtual void set_volumetric(bool) {}
	 virtual void set_volumetric_quality(float) {}
	 virtual void set_volumetric_intensity(float) {}
	 virtual void set_volumetric_distance(float) {}
	 virtual void set_indirect(bool) {};
	 virtual void set_position(const Fvector& P) {}
	 virtual void set_rotation(const Fvector& D, const Fvector& R) {}
	 virtual void set_cone(float angle) {}
	 virtual void set_range(float R) {}
	 virtual void set_virtual_size(float R) {}
	 virtual void set_texture(LPCSTR name) {}
	 virtual void set_color(const Fcolor& C) {}
	 virtual void set_color(float r, float g, float b) {}
	 virtual void set_hud_mode(bool b) {}
	 virtual bool get_hud_mode() {
		 return false;
	 }
	 virtual ~RLight() {}
 };
 IRender_Light* CRender::light_create() { return xr_new< RLight>(); }
 void CRender::light_destroy(IRender_Light* p_) {  }



 class RGlow : public IRender_Glow
 {
 public:
 public:
	 RGlow() {}
	 virtual	~RGlow() {}

	 virtual void					set_active(bool) {}
	 virtual bool					get_active() { return false; }
	 virtual void					set_position(const Fvector& P) { return ; }
	 virtual void					set_direction(const Fvector& P) { return ; }
	 virtual void					set_radius(float			R) { return ; }
	 virtual void					set_texture(LPCSTR			name) { return ; }
	 virtual void					set_color(const Fcolor& C) { return ; }
	 virtual void					set_color(float r, float g, float b) { return ; }
	 virtual void					spatial_move() { return ; }
 };

 IRender_Glow* CRender::glow_create() { return xr_new< RGlow>(); }
 void CRender::glow_destroy(IRender_Glow* p_) {  }
 void CRender::model_Logging(BOOL bEnable) {}
#if defined(USE_DX10) || defined(USE_DX11)
// XrRender/Private/Texture.cpp carries the D3D9 version but is swapped out for
// the DX11 layer, which only implements texture_load. On D3D11 "software" just
// means a CPU-readable copy, and that is exactly what the staging flag asks for.
ID3DBaseTexture* CRender::texture_load_software(LPCSTR fname, u32& mem_size)
{
	return texture_load(fname, mem_size, true);
}
#endif

void CRender::models_Prefetch() {}
void CRender::models_Clear(BOOL b_complete) {}
void CRender::Screenshot(ScreenshotMode mode , LPCSTR name ) {}
void CRender::Screenshot(ScreenshotMode mode, CMemoryWriter& memory_writer) {}
void CRender::ScreenshotAsyncBegin() {}
void CRender::ScreenshotAsyncEnd(CMemoryWriter& memory_writer) {}
u32 CRender::memory_usage() { return 0; }