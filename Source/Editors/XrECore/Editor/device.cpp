//---------------------------------------------------------------------------
#include "stdafx.h"
#pragma hdrstop
#include "gamefont.h"
#include <sal.h>
#include "ImageManager.h"
#include "ui_main.h"
#include "render.h"
#include "../Engine/XrGameMaterialLibraryEditors.h"
#include "ResourceManager.h"
#include "UI_ToolsCustom.h"

CEditorRenderDevice 	*	EDevice;

extern int	rsDVB_Size;
extern int	rsDIB_Size;

CStatsPhysics* _BCL			CEditorRenderDevice::StatPhysics() { return Statistic; }
void	   _BCL			CEditorRenderDevice::AddSeqFrame(pureFrame* f, bool mt) { seqFrame.Add(f, REG_PRIORITY_LOW); }
void	   _BCL			CEditorRenderDevice::RemoveSeqFrame(pureFrame* f) { seqFrame.Remove(f); }

ENGINE_API BOOL g_bRendering;
//---------------------------------------------------------------------------
CEditorRenderDevice::CEditorRenderDevice()
{
	RadiusRender = 400;
	psDeviceFlags.assign(rsStatistic|rsFilterLinear|rsFog|rsDrawGrid);
// dynamic buffer size
	rsDVB_Size		= 2048;
	rsDIB_Size		= 2048;
// default initialization
    m_ScreenQuality = 1.f;
	dwMaximized = 0;
    dwWidth 		= dwHeight 	= 256;
	dwRealWidth = dwRealHeight = 256;
	mProject.identity();
    mFullTransform.identity();
    mView.identity	();
	m_WireShader	= 0;
	m_SelectionShader = 0;

    b_is_Ready 			= FALSE;
	b_is_Active			= FALSE;

	// Engine flow-control
	fTimeDelta		= 0;
	fTimeGlobal		= 0;
	dwTimeDelta		= 0;
	dwTimeGlobal	= 0;

	dwFillMode		= D3DFILL_SOLID;
    dwShadeMode		= D3DSHADE_GOURAUD;

    m_CurrentShader	= 0;
    pSystemFont		= 0;

	fASPECT 		= 1.f;
	fFOV 			= 60.f;
    dwPrecacheFrame = 0;
	GameMaterialLibraryEditors = xr_new<XrGameMaterialLibraryEditors>();
	GameMaterialLibrary = GameMaterialLibraryEditors;
}

CEditorRenderDevice::~CEditorRenderDevice(){
	VERIFY(!b_is_Ready);
	xr_delete(GameMaterialLibrary);
	GameMaterialLibraryEditors = nullptr;
}

//extern void Surface_Init();
#include "../../../xrAPI/xrAPI.h"
#include "../../../xrRender/Private/dxRenderFactory.h"
#include "../../../xrRender/Private/dxUIRender.h"
#include "../../../xrRender/Private/dxDebugRender.h"
typedef void __cdecl ttapi_Done_func(void);
void CEditorRenderDevice::Initialize()
{
//	m_Camera.Reset();
	{
		hPSGP = LoadLibrary("xrCPU_Pipe.dll");
		R_ASSERT(hPSGP);
		xrBinder* bindCPU = (xrBinder*)GetProcAddress(hPSGP, "xrBind_PSGP");	R_ASSERT(bindCPU);
		bindCPU(&PSGP, CPU::ID.feature);
	}

    m_DefaultMat.set(1,1,1);
//	Surface_Init();

	RenderFactory = &RenderFactoryImpl;
	UIRender = &UIRenderImpl;
#ifdef DEBUG
	DRender = &DebugRenderImpl;
#endif

	// game materials
	//GameMaterialLibraryEditors->Load	();

	// compiler shader
    string_path fn;
    FS.update_path(fn,_game_data_,"shaders_xrlc.xr");
    if (FS.exist(fn)){
    	ShaderXRLC.Load(fn);
    }else{
    	ELog.DlgMsg(mtInformation,"Can't find file '%s'",fn);
    }


	CreateWindow();


	// Startup shaders
	Create				();

    ::RImplementation.Initialize();
	UIRenderImpl.CreateUIGeom();

	Resize(EPrefs->start_w, EPrefs->start_h, EPrefs->start_maximized);
	HW.updateWindowProps(m_hWnd);


	::ShowWindow(m_hWnd, EPrefs->start_maximized? SW_SHOWMAXIMIZED: SW_SHOWDEFAULT);
}

void CEditorRenderDevice::ShutDown()
{
	UIRenderImpl.DestroyUIGeom();
	::RImplementation.ShutDown	();

	ShaderXRLC.Unload	();
	//GameMaterialLibraryEditors->Unload		();

	// destroy context
	Destroy				();
	xr_delete			(pSystemFont);

	if (hPSGP)
	{
		ttapi_Done_func* ttapi_Done = (ttapi_Done_func*)GetProcAddress(hPSGP, "ttapi_Done");	R_ASSERT(ttapi_Done);
		if (ttapi_Done)
			ttapi_Done();

		FreeLibrary(hPSGP);
		hPSGP = 0;
		ZeroMemory(&PSGP, sizeof(PSGP));
	}
	// destroy shaders
//	PSLib.xrShutDown	();
}

void CEditorRenderDevice::InitTimer(){
	Timer_MM_Delta	= 0;
	{
		u32 time_mm			= clock	();
		while (clock()==time_mm);			// wait for next tick
		u32 time_system		= clock();
		u32 time_local		= TimerAsync	();
		Timer_MM_Delta			= time_system-time_local;
	}
}
//---------------------------------------------------------------------------
void CEditorRenderDevice::RenderNearer(float n){
    mProject._43=m_fNearer-n;
    RCache.set_xform_project(mProject);
}
void CEditorRenderDevice::ResetNearer(){
    mProject._43=m_fNearer;
    RCache.set_xform_project(mProject);
}
//---------------------------------------------------------------------------
bool CEditorRenderDevice::Create()
{
	if (b_is_Ready)	return false;

    EStatistic			= xr_new<CEStats>();
	Statistic = EStatistic;
	ELog.Msg(mtInformation,"Starting RENDER device...");


	HW.CreateDevice		(m_hWnd, true);
	if (UI)
	{	
		string_path 		ini_path;
		string_path			ini_name;
		xr_strcpy			(ini_name, UI->EditorName());
		xr_strcat			(ini_name, "_imgui.ini");
		FS.update_path(ini_path, "$local_root$", ini_name);
		if (!FS.exist(ini_path))UI->ResetUI();
#if defined(USE_DX11)
		UI->Initialize(m_hWnd, HW.pDevice, HW.pContext, ini_path);
#else
		UI->Initialize(m_hWnd, HW.pDevice, ini_path);
#endif
	}
	
	// after creation
	dwFrame				= 0;

	string_path 		sh;
    FS.update_path		(sh,_game_data_,"shaders.xr");

    IReader* F			= 0;
	if (FS.exist(sh))
		F				= FS.r_open(0,sh);
	Resources			= xr_new<CResourceManager>	();

    // if build options - load textures immediately
    if (strstr(Core.Params,"-build")||strstr(Core.Params,"-ebuild"))
        EDevice->Resources->DeferredLoad(FALSE);

    _Create				(F);
	FS.r_close			(F);

	ELog.Msg			(mtInformation, "D3D: initialized");

	return true;
}

//---------------------------------------------------------------------------
void CEditorRenderDevice::Destroy(){
	if (!b_is_Ready) return;
	ELog.Msg( mtInformation, "Destroying Direct3D...");

	HW.Validate			();

	// before destroy
	_Destroy			(FALSE);
	xr_delete			(Resources);

	UI->Destroy();
	// real destroy
	HW.DestroyDevice	();

	ELog.Msg( mtInformation, "D3D: device cleared" );
    xr_delete			(Statistic);
}
//---------------------------------------------------------------------------
void CEditorRenderDevice::_SetupStates()
{
	HW.Caps.Update();
#if !defined(USE_DX10) && !defined(USE_DX11)
	for (u32 i=0; i<HW.Caps.raster.dwStages; i++){
		float fBias = -1.f;
		CHK_DX(HW.pDevice->SetSamplerState( i, D3DSAMP_MIPMAPLODBIAS, *((LPDWORD) (&fBias))));
	}
#endif
	// SetRS/SetSS are editor wrappers - under D3D11 they only record state that
	// the fixed-function emulation later folds into the shader constants
	EDevice->SetRS(D3DRS_DITHERENABLE,	TRUE				);
    EDevice->SetRS(D3DRS_COLORVERTEX,		TRUE				);
    EDevice->SetRS(D3DRS_STENCILENABLE,	FALSE				);
    EDevice->SetRS(D3DRS_ZENABLE,			TRUE				);
    EDevice->SetRS(D3DRS_SHADEMODE,		D3DSHADE_GOURAUD	);
	EDevice->SetRS(D3DRS_CULLMODE,		D3DCULL_CCW			);
	EDevice->SetRS(D3DRS_ALPHAFUNC,		D3DCMP_GREATER		);
	EDevice->SetRS(D3DRS_LOCALVIEWER,		TRUE				);
    EDevice->SetRS(D3DRS_NORMALIZENORMALS,TRUE				);

	EDevice->SetRS(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
	EDevice->SetRS(D3DRS_SPECULARMATERIALSOURCE,D3DMCS_MATERIAL);
	EDevice->SetRS(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
	EDevice->SetRS(D3DRS_EMISSIVEMATERIALSOURCE,D3DMCS_COLOR1	);

    ResetMaterial();
}
//---------------------------------------------------------------------------
void CEditorRenderDevice::_Create(IReader* F)
{
	b_is_Ready				= TRUE;

	// General Render States
    _SetupStates		();
    
    RCache.OnDeviceCreate		();
	Resources->OnDeviceCreate	(F);
	::RImplementation.OnDeviceCreate	();

    m_WireShader.create			("editor\\wire");
    m_SelectionShader.create	("editor\\selection");

	// signal another objects
    UI->OnDeviceCreate			();           
//.	seqDevCreate.Process		(rp_DeviceCreate);

	pSystemFont					= xr_new<CGameFont>("hud_font_small");
//	pSystemFont					= xr_new<CGameFont>("hud_font_medium");
}

void CEditorRenderDevice::_Destroy(BOOL	bKeepTextures)
{
	xr_delete					(pSystemFont);

	b_is_Ready 						= FALSE;
    m_CurrentShader				= 0;

    UI->OnDeviceDestroy			();

	m_WireShader.destroy		();
	m_SelectionShader.destroy	();

//.	seqDevDestroy.Process		(rp_DeviceDestroy);

	::RImplementation.Models->OnDeviceDestroy	();

	Resources->OnDeviceDestroy	(bKeepTextures);

	RCache.OnDeviceDestroy		();
	::RImplementation.OnDeviceDestroy	();
}

//---------------------------------------------------------------------------
void  CEditorRenderDevice::Resize(int w, int h, bool maximized)
{
	if (dwRealWidth == w && dwRealHeight == h&& dwMaximized == maximized)return;
    m_RenderArea	= w*h;

	dwRealWidth = w;
	dwRealHeight = h;
	dwMaximized = maximized;

    Reset			(false);
    UI->RedrawScene	();
}

TEditorDeviceResetProc	g_pOnEditorDeviceResetBegin	= nullptr;
TEditorDeviceResetProc	g_pOnEditorDeviceResetEnd	= nullptr;

void CEditorRenderDevice::Reset  	(bool )
{
    u32 tm_start			= TimerAsync();
	// out-of-XrECore owners of D3DPOOL_DEFAULT resources go first: HW.Reset
	// R_CHKs the result and a single surviving surface makes Reset() fail
	if (g_pOnEditorDeviceResetBegin)	g_pOnEditorDeviceResetBegin();
    Resources->reset_begin	();
	UI->ResetBegin();
    Memory.mem_compact		();
#if defined(USE_DX10) || defined(USE_DX11)
    HW.m_ChainDesc.BufferDesc.Width		= dwRealWidth;
    HW.m_ChainDesc.BufferDesc.Height	= dwRealHeight;
    HW.Reset				(m_hWnd);
    dwRealWidth					= HW.m_ChainDesc.BufferDesc.Width;
    dwRealHeight				= HW.m_ChainDesc.BufferDesc.Height;
#else
    HW.DevPP.BackBufferWidth= dwRealWidth;
    HW.DevPP.BackBufferHeight= dwRealHeight;
    HW.Reset				(m_hWnd);
    dwRealWidth					= HW.DevPP.BackBufferWidth;
    dwRealHeight				= HW.DevPP.BackBufferHeight;
#endif
//		fWidth_2			= float(dwRealWidth/2);
//		fHeight_2			= float(dwRealHeight/2);
    Resources->reset_end	();
	UI->ResetEnd();
	if (g_pOnEditorDeviceResetEnd)		g_pOnEditorDeviceResetEnd();
    _SetupStates			();
    u32 tm_end				= TimerAsync();
    Msg						("*** RESET [%d ms]",tm_end-tm_start);
}

bool CEditorRenderDevice::Begin	()
{
	VERIFY(b_is_Ready);
	mFullTransform_saved = mFullTransform;
	mProject_saved = mProject;
	mView_saved = mView;
	vCameraPosition_saved = vCameraPosition;
	HW.Validate		();
#if defined(USE_DX10) || defined(USE_DX11)
	// D3D11 has no lost-device protocol and no scene begin/end: bind the
	// backbuffer for this frame and clear it directly.
    VERIFY 					(FALSE==g_bRendering);
	RCache.set_RT			(HW.pBaseRT);
	RCache.set_ZB			(HW.pBaseZB);

	u32			clr			= EPrefs ? EPrefs->scene_clear_color : 0x0;
	// Fcolor lays out r,g,b,a as four consecutive floats - exactly the
	// FLOAT[4] ClearRenderTargetView wants
	Fcolor		cc4;
	cc4.set					(clr);
	HW.pContext->ClearRenderTargetView	(HW.pBaseRT, &cc4.r);
	HW.pContext->ClearDepthStencilView	(HW.pBaseZB,
		D3D_CLEAR_DEPTH | (HW.Caps.bStencil ? D3D_CLEAR_STENCIL : 0), 1.0f, 0);
#else
	HRESULT	_hr		= HW.pDevice->TestCooperativeLevel();
    if (FAILED(_hr))
	{
		// If the device was lost, do not render until we get it back
		if		(D3DERR_DEVICELOST==_hr)		{
			Sleep	(33);
			return	FALSE;
		}

		// Check if the device is ready to be reset
		if		(D3DERR_DEVICENOTRESET==_hr)
		{
			Reset	(false);
		}
	}

    VERIFY 					(FALSE==g_bRendering);
	CHK_DX					(HW.pDevice->BeginScene());
	CHK_DX(HW.pDevice->Clear(0,0,
		D3DCLEAR_ZBUFFER|D3DCLEAR_TARGET|
		(HW.Caps.bStencil?D3DCLEAR_STENCIL:0),
		EPrefs?EPrefs->scene_clear_color:0x0,1,0
		));
#endif
	RCache.OnFrameBegin		();
	g_bRendering = 	TRUE;
	return		TRUE;
}

//---------------------------------------------------------------------------
void CEditorRenderDevice::End()
{
	VERIFY(HW.pDevice);
	VERIFY(b_is_Ready);
	g_bRendering = 	FALSE;
	// end scene
	RCache.OnFrameEnd();
#if defined(USE_DX10) || defined(USE_DX11)
	HW.m_pSwapChain->Present( 0, 0 );
#else
    CHK_DX(HW.pDevice->EndScene());

	CHK_DX(HW.pDevice->Present( NULL, NULL, NULL, NULL ));
#endif

}

void CEditorRenderDevice::UpdateView()
{
// set camera matrix
	if (!Tools->UpdateCamera())
	{
		m_Camera.GetView(mView);
	}
    RCache.set_xform_view(mView);
    mFullTransform.mul(mProject,mView);

// frustum culling sets
    ::Render->ViewBase.CreateFromMatrix(mFullTransform,FRUSTUM_P_ALL);
}

void CEditorRenderDevice::FrameMove()
{
	dwFrame++;

	// Timer
    float fPreviousFrameTime = Timer.GetElapsed_sec(); Timer.Start();	// previous frame
    fTimeDelta = 0.1f * fTimeDelta + 0.9f*fPreviousFrameTime;			// smooth random system activity - worst case ~7% error
    if (fTimeDelta>.1f) fTimeDelta=.1f;									// limit to 15fps minimum

    fTimeGlobal		= TimerGlobal.GetElapsed_sec(); //float(qTime)*CPU::cycles2seconds;
    dwTimeGlobal	= TimerGlobal.GetElapsed_ms	();	//u32((qTime*u64(1000))/CPU::cycles_per_second);
    dwTimeDelta		= iFloor(fTimeDelta*1000.f+0.5f);
    dwTimeContinual	= dwTimeGlobal;

	if (!Tools->UpdateCamera())
	{
		m_Camera.Update(fTimeDelta);
	}

    // process objects
	seqFrame.Process(rp_Frame);
}

#if defined(USE_DX11)
void CEditorRenderDevice::ApplyFFConstants()
{
	// TEXTUREFACTOR used to tint fixed-function output through a texture stage;
	// the editor shaders take it as a uniform instead. Named lookups miss
	// silently when a shader has no such constant, so this is safe for all of them.
	const u32 tf	= ff.render_state[u32(D3DRS_TEXTUREFACTOR) & (SEditorFixedFunc::RS_MAX-1)];
	Fcolor c;		c.set(tf);
	RCache.set_c	("tfactor", c.r, c.g, c.b, c.a);

	// Pre-transformed (POSITIONT) geometry has no fixed-function T&L left to
	// divide by the viewport, so the vertex shader does it from these.
	const float w	= float(dwWidth), h = float(dwHeight);
	RCache.set_c	("screen_res", w, h, w>0.f?1.f/w:0.f, h>0.f?1.f/h:0.f);
}
#endif

void CEditorRenderDevice::DP(D3DPRIMITIVETYPE pt, ref_geom geom, u32 vBase, u32 pc)
{
	ref_shader S 			= m_CurrentShader?m_CurrentShader:m_WireShader;
    u32 dwRequired			= S->E[0]->passes.size();
    RCache.set_Geometry		(geom);
    for (u32 dwPass = 0; dwPass<dwRequired; dwPass++){
    	RCache.set_Shader	(S,dwPass);
#if defined(USE_DX11)
		ApplyFFConstants	();
#endif
		RCache.Render		(pt,vBase,pc);
    }
}

void CEditorRenderDevice::DIP(D3DPRIMITIVETYPE pt, ref_geom geom, u32 baseV, u32 startV, u32 countV, u32 startI, u32 PC)
{
	ref_shader S 			= m_CurrentShader?m_CurrentShader:m_WireShader;
    u32 dwRequired			= S->E[0]->passes.size();
    RCache.set_Geometry		(geom);
    for (u32 dwPass = 0; dwPass<dwRequired; dwPass++){
    	RCache.set_Shader	(S,dwPass);
#if defined(USE_DX11)
		ApplyFFConstants	();
#endif
		RCache.Render		(pt,baseV,startV,countV,startI,PC);
    }
}

void CEditorRenderDevice::ReloadTextures()
{
	UI->SetStatus("Reload textures...");
	Resources->ED_UpdateTextures(0);
	UI->SetStatus("");
}

void CEditorRenderDevice::UnloadTextures()
{
}

void CEditorRenderDevice::Reset(IReader* F, BOOL bKeepTextures)
{
	CTimer tm;
    tm.Start();
	_Destroy		(bKeepTextures);
	_Create			(F);
	Msg				("*** RESET [%d ms]",tm.GetElapsed_ms());
}

void CEditorRenderDevice::time_factor(float v)
{
	 Timer.time_factor(v);
	 TimerGlobal.time_factor(v);
}

//------------------------------------------------------------------------------
// GPU preference — stored where Windows' own "Graphics settings" page keeps it,
// keyed by the full executable path. Applied by the OS at process start.
//------------------------------------------------------------------------------
static const char* kGpuPrefKey = "Software\\Microsoft\\DirectX\\UserGpuPreferences";

static bool GetSelfPath(string_path& dst)
{
	return ::GetModuleFileNameA(NULL, dst, sizeof(string_path)) != 0;
}

EGpuPreference GetGpuPreference()
{
	string_path self;
	if (!GetSelfPath(self)) return gpuAuto;

	HKEY key;
	if (::RegOpenKeyExA(HKEY_CURRENT_USER, kGpuPrefKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return gpuAuto;

	char value[64] = {};
	DWORD size = sizeof(value) - 1, type = 0;
	const LSTATUS res = ::RegQueryValueExA(key, self, NULL, &type, (LPBYTE)value, &size);
	::RegCloseKey(key);

	if (res != ERROR_SUCCESS || type != REG_SZ) return gpuAuto;
	if (strstr(value, "GpuPreference=2")) return gpuPerformance;
	if (strstr(value, "GpuPreference=1")) return gpuPowerSaving;
	return gpuAuto;
}

bool SetGpuPreference(EGpuPreference pref)
{
	string_path self;
	if (!GetSelfPath(self)) return false;

	HKEY key;
	if (::RegCreateKeyExA(HKEY_CURRENT_USER, kGpuPrefKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
		return false;

	LSTATUS res;
	if (gpuAuto == pref)
	{
		// no value at all == "let Windows decide"
		res = ::RegDeleteValueA(key, self);
		if (ERROR_FILE_NOT_FOUND == res) res = ERROR_SUCCESS;
	}
	else
	{
		string64 value;
		xr_sprintf(value, "GpuPreference=%d;", int(pref));
		res = ::RegSetValueExA(key, self, 0, REG_SZ, (const BYTE*)value, DWORD(xr_strlen(value) + 1));
	}

	::RegCloseKey(key);
	return ERROR_SUCCESS == res;
}

LPCSTR GetActiveGpuName()
{
	static string256 name = {};
	if (!name[0])
	{
#if defined(USE_DX10) || defined(USE_DX11)
		// DXGI reports the description as wide chars, so it has to be narrowed
		DXGI_ADAPTER_DESC desc = {};
		if (HW.m_pAdapter && SUCCEEDED(HW.m_pAdapter->GetDesc(&desc)))
			::WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, name, sizeof(name), NULL, NULL);
		else
			xr_strcpy(name, "unknown");
#else
		D3DADAPTER_IDENTIFIER9 id = {};
		if (HW.pD3D && SUCCEEDED(HW.pD3D->GetAdapterIdentifier(HW.DevAdapter, 0, &id)))
			xr_strcpy(name, id.Description);
		else
			xr_strcpy(name, "unknown");
#endif
	}
	return name;
}

int GpuAdapterCount()
{
	return (int)CHW::AdapterNames.size();
}

LPCSTR GpuAdapterName(int index)
{
	if (index < 0 || index >= (int)CHW::AdapterNames.size())	return "";
	return CHW::AdapterNames[index].c_str();
}

void SetPreferredGpu(LPCSTR description)
{
	strncpy_s(CHW::PreferredAdapter, description ? description : "", _TRUNCATE);
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void CEditorRenderDevice::CreateWindow()
{
	// Editors are DPI-unaware by default, so Windows bitmap-stretches them on
	// scaled displays (blurry UI). Opt into Per-Monitor V2 before creating the
	// window; resolve APIs dynamically to keep older systems working.
	float dpi_scale = 1.f;
	if (HMODULE user32 = ::GetModuleHandleA("user32.dll"))
	{
		typedef BOOL(WINAPI* SetCtxFn)(HANDLE);
		typedef BOOL(WINAPI* SetAwareFn)();
		typedef UINT(WINAPI* GetDpiFn)();
		SetCtxFn set_ctx = (SetCtxFn)::GetProcAddress(user32, "SetProcessDpiAwarenessContext");
		SetAwareFn set_aware = (SetAwareFn)::GetProcAddress(user32, "SetProcessDPIAware");
		// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (DPI_AWARENESS_CONTEXT)-4
		const bool aware = set_ctx ? !!set_ctx((HANDLE)-4) : (set_aware && set_aware());
		if (aware)
			if (GetDpiFn get_dpi = (GetDpiFn)::GetProcAddress(user32, "GetDpiForSystem"))
				dpi_scale = float(get_dpi()) / 96.f;
	}

	m_WC = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, TEXT("XFined Editor"), NULL };
	::RegisterClassEx(&m_WC);
	m_hWnd= ::CreateWindowA(m_WC.lpszClassName, TEXT("XFined Editor"), WS_OVERLAPPEDWINDOW, 100, 100, int(1280 * dpi_scale), int(800 * dpi_scale), NULL, NULL, m_WC.hInstance, NULL);

	::UpdateWindow(m_hWnd);
}
void CEditorRenderDevice::DestryWindow()
{
	::DestroyWindow(m_hWnd);
	::UnregisterClass(m_WC.lpszClassName, m_WC.hInstance);
}
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_ACTIVATE:
	{
		u16 fActive = LOWORD(wParam);
		BOOL fMinimized = (BOOL)HIWORD(wParam);
		BOOL bActive = ((fActive != WA_INACTIVE) && (!fMinimized)) ? TRUE : FALSE;
		if (bActive != EDevice->b_is_Active)
		{
			EDevice->b_is_Active = bActive;

			if (EDevice->b_is_Active)
			{
				if (UI)UI->OnAppActivate();
			}
			else
			{
				
				if (UI)UI->OnAppDeactivate();
			}
		}
	}
	break;
	}
	if (UI &&UI->WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_KEYDOWN: 
	case WM_SYSKEYDOWN:
		if(UI)UI->KeyDown(wParam,UI->GetShiftState());
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (UI)UI->KeyUp(wParam, UI->GetShiftState());
		break;
	
	case WM_SIZE:

		if (UI && HW.pDevice)
		{
			UI->Resize(LOWORD(lParam), HIWORD(lParam), wParam == SIZE_MAXIMIZED);
		}
		/*if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED)
		{
			g_d3dpp.BackBufferWidth = ;
			g_d3dpp.BackBufferHeight = ;
			ResetDevice();
		}*/
		return 0;
	
	case WM_SYSCOMMAND:

		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
		{
			return 0;
		}
		break;
	case WM_DESTROY:
		::PostQuitMessage(0);
		return 0;
	}
	return ::DefWindowProc(hWnd, msg, wParam, lParam);
}