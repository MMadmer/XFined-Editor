#pragma once

#include "../../../xrengine/XrDeviceInterface.h"
#include "ui_camera.h"
#include "../../../xrRender/Private/hwcaps.h"
#include "../../../xrRender/Private/hw.h"
#include "../../../xrEngine/pure.h"
#include "../../../xrCore/ftimer.h"
#include "estats.h"
#include "../../../xrEngine/shader_xrlc.h"
#include "../../../xrRender/Private/shader.h"
#include "../../../xrRender/Private/R_Backend.h"


//---------------------------------------------------------------------------
// refs
class CGameFont;
class CInifile;
class CResourceManager;
#undef CreateWindow

#if defined(USE_DX11)
//------------------------------------------------------------------------------
// Fixed-function state the editor still speaks in.
//
// D3D11 dropped the FFP entirely, but the editor's drawing code is written
// against it in ~70 places (grid, gizmo, selection, debug draw, previews).
// Rather than rewriting every caller at once, the setters record into this
// block and the shader-based drawing path reads it back. Indices are masked, so
// a stray state id can never write out of bounds.
//------------------------------------------------------------------------------
struct SEditorFixedFunc
{
	enum { RS_MAX = 256, SS_MAX = 16, SAMPLERS = 8, LIGHTS = 8 };

	u32			render_state[RS_MAX];
	u32			sampler_state[SAMPLERS][SS_MAX];
	Flight		light[LIGHTS];
	bool		light_enabled[LIGHTS];
	Fmaterial	material;

	SEditorFixedFunc()	{ reset(); }
	void reset()
	{
		ZeroMemory(render_state,	sizeof(render_state));
		ZeroMemory(sampler_state,	sizeof(sampler_state));
		ZeroMemory(light,			sizeof(light));
		ZeroMemory(light_enabled,	sizeof(light_enabled));
		ZeroMemory(&material,		sizeof(material));
	}
};

// Rasterizer fill mode. A free function, and declared here rather than inside
// the class, because SetRS() below needs it while the render layer's state
// manager is not visible from this header.
ECORE_API void			EDevice_SetFillMode(u32 d3d9_fill);
#endif

//------------------------------------------------------------------------------
class ECORE_API CEditorRenderDevice :
	public XrDeviceInterface
{
    friend class 			CUI_Camera;
    friend class 			TUI;
	HMODULE					hPSGP;
    float 					m_fNearer;

	//u32						Timer_MM_Delta;
	//CTimer					Timer;
	//CTimer					TimerGlobal;

    ref_shader				m_CurrentShader;

    void					_SetupStates();
	void					_Create		(IReader* F);
	void					_Destroy	(BOOL	bKeepTextures);
public:
    ref_shader				m_WireShader;
    ref_shader				m_SelectionShader;

    Fmaterial				m_DefaultMat;
#if defined(USE_DX11)
	// recorded fixed-function state, consumed by the shader drawing path
	SEditorFixedFunc		ff;
#endif
public:
	float RadiusRender;
   // u32 					dwWidth, dwHeight;
	u32 					dwRealWidth, dwRealHeight;
    float					m_RenderArea;
    float 					m_ScreenQuality;

	u32 					dwFillMode;
    u32						dwShadeMode;
public:
//   HWND 					m_hWnd;


//	u32						dwFrame;
//	u32						dwPrecacheFrame;

//	BOOL					b_is_Ready;
//	BOOL					b_is_Active;

	// Engine flow-control
	//float					fTimeDelta;
	//float					fTimeGlobal;
	//u32						dwTimeDelta;
	//u32						dwTimeGlobal;
 //   u32						dwTimeContinual;

    // camera
	CUI_Camera 				m_Camera;

 //   Fvector					vCameraPosition;
 //   Fvector					vCameraDirection;
 //   Fvector					vCameraTop;
 //   Fvector					vCameraRight;
 //   
	//Fmatrix					mView;
	//Fmatrix 				mProjection;
	//Fmatrix					mFullTransform;

 //   float					fFOV;
	//float					fASPECT;

	// Dependent classes
	CResourceManager*		Resources;	  
	CEStats*				EStatistic;

	CGameFont* 				pSystemFont;

	// registrators
//	CRegistrator <pureDeviceDestroy>	seqDevDestroy;
//	CRegistrator <pureDeviceCreate>		seqDevCreate;

	//CRegistrator <pureFrame>					seqFrame;				
	//CRegistrator <pureRender>					seqRender;
	//CRegistrator <pureAppStart>					seqAppStart;
	//CRegistrator <pureAppEnd>					seqAppEnd;
	//CRegistrator <pureAppActivate	>			seqAppActivate;
	//CRegistrator <pureAppDeactivate	>			seqAppDeactivate;
public:
							CEditorRenderDevice 	();
    virtual 				~CEditorRenderDevice	();

	
	virtual  bool			Paused()const { return FALSE; };
    void					time_factor		(float);
	bool 					Create			();
	void 					Destroy			();
    void 					Resize			(int w, int h,bool maximized);
	void 					ReloadTextures	();
	void 					UnloadTextures	();

    void 					RenderNearer	(float f_Near);
    void 					ResetNearer		();
	bool 					Begin();
	void 					End				();

	void 					Initialize		(void);
	void 					ShutDown		(void);
	void 					Reset			(IReader* F, BOOL bKeepTextures);

	virtual void DumpResourcesMemoryUsage() {}
    IC float				GetRenderArea	(){return m_RenderArea;}
	// Sprite rendering
	IC float 				_x2real			(float x)
    { return (x+1)*dwWidth*0.5f;	}
	IC float 				_y2real			(float y)
    { return (y+1) * dwHeight * 0.5f;}

	// draw
	void			   		SetShader		(ref_shader sh){m_CurrentShader = sh;}
#if defined(USE_DX11)
	// Pushes the recorded fixed-function bits the editor shaders take as
	// uniforms. Has to run after set_Shader - constants are per-pass.
	void					ApplyFFConstants();
#endif
	void			   		DP				(D3DPRIMITIVETYPE pt, ref_geom geom, u32 startV, u32 pc);
	void 					DIP				(D3DPRIMITIVETYPE pt, ref_geom geom, u32 baseV, u32 startV, u32 countV, u32 startI, u32 PC);

#if defined(USE_DX11)
	// D3D11 has no fixed-function pipeline: no render/sampler state setters, no
	// lights, no material. The editor calls these from ~70 places, so instead of
	// touching all of them the state is recorded here and the shader-based
	// drawing path consumes it. Callers keep their meaning; only the backend
	// moved. See SEditorFixedFunc below.
	IC void					SetRS			(D3DRENDERSTATETYPE p1, u32 p2)
	{
		ff.render_state[u32(p1) & (SEditorFixedFunc::RS_MAX-1)] = p2;
		// The fill mode is NOT fixed-function state - it is rasterizer state,
		// and D3D11 still has it. Recording it alone left wireframe/point dead
		// in the viewport, so forward it to the state manager as well.
		if (D3DRS_FILLMODE == p1) EDevice_SetFillMode(p2);
	}
	IC void					SetSS			(u32 sampler, D3DSAMPLERSTATETYPE type, u32 value)
	{ if (sampler < SEditorFixedFunc::SAMPLERS) ff.sampler_state[sampler][u32(type) & (SEditorFixedFunc::SS_MAX-1)] = value; }

	// light&material
	IC void					LightEnable		(u32 dwLightIndex, BOOL bEnable)
	{ if (dwLightIndex < SEditorFixedFunc::LIGHTS) ff.light_enabled[dwLightIndex] = !!bEnable; }
	IC void					SetLight		(u32 dwLightIndex, Flight& lpLight)
	{ if (dwLightIndex < SEditorFixedFunc::LIGHTS) ff.light[dwLightIndex] = lpLight; }
	IC void					SetMaterial		(Fmaterial& mat)	{ ff.material = mat; }
	IC void					ResetMaterial	()					{ ff.material = m_DefaultMat; }
#else
    IC void					SetRS			(D3DRENDERSTATETYPE p1, u32 p2)
    { VERIFY(b_is_Ready); CHK_DX(HW.pDevice->SetRenderState(p1,p2)); }
    IC void					SetSS			(u32 sampler, D3DSAMPLERSTATETYPE type, u32 value)
    { VERIFY(b_is_Ready); CHK_DX(HW.pDevice->SetSamplerState(sampler,type,value)); }

    // light&material
    IC void					LightEnable		(u32 dwLightIndex, BOOL bEnable)
    { CHK_DX(HW.pDevice->LightEnable(dwLightIndex, bEnable));}
    IC void					SetLight		(u32 dwLightIndex, Flight& lpLight)
    { CHK_DX(HW.pDevice->SetLight(dwLightIndex, (D3DLIGHT9*)&lpLight));}
	IC void					SetMaterial		(Fmaterial& mat)
    { CHK_DX(HW.pDevice->SetMaterial((D3DMATERIAL9*)&mat));}
	IC void					ResetMaterial	()
    { CHK_DX(HW.pDevice->SetMaterial((D3DMATERIAL9*)&m_DefaultMat));}
#endif

	// update
    void					UpdateView		();
	void					FrameMove		();

    bool					MakeScreenshot	(U32Vec& pixels, u32 width, u32 height);

	void 					InitTimer		();
	// Mode control
	virtual void	Pause(BOOL bOn, BOOL bTimer, BOOL bSound, LPCSTR reason) {}
	virtual void PreCache(u32 amount, bool b_draw_loadscreen, bool b_wait_user_input) {}
	virtual void Clear() {}
public:
    Shader_xrLC_LIB			ShaderXRLC;
private:
	virtual		CStatsPhysics* _BCL			StatPhysics();
	virtual				void	   _BCL			AddSeqFrame(pureFrame* f, bool mt);
	virtual				void	   _BCL			RemoveSeqFrame(pureFrame* f);
private:
	WNDCLASSEX m_WC;
public:
	void CreateWindow();
	void DestryWindow();
	virtual			bool				IsEditorMode() { return true; }
	virtual void Reset(bool precache);
};

extern ECORE_API CEditorRenderDevice* EDevice;

#if defined(USE_DX11)
// The engine backend calls this after every set_Element (REDITOR only), so the
// recorded fixed-function state reaches shaders on EVERY draw path - the model
// pool included, which never goes through EDevice->DP/DIP. A free function
// because R_Backend_Runtime.h cannot see the class.
ECORE_API void			EDevice_PushFFConstants();
#endif

// Device-reset notifications for code living outside XrECore. D3D9 refuses to
// Reset() while any D3DPOOL_DEFAULT resource is alive, so a panel that owns a
// render target of its own has to drop it here - CResourceManager only knows
// about the CRTs it created itself.
typedef void (*TEditorDeviceResetProc)();
extern ECORE_API TEditorDeviceResetProc g_pOnEditorDeviceResetBegin;
extern ECORE_API TEditorDeviceResetProc g_pOnEditorDeviceResetEnd;

// video
enum {
	rsFilterLinear		= (1ul<<20ul),
    rsEdgedFaces		= (1ul<<21ul),
	rsRenderTextures	= (1ul<<22ul),
	rsLighting			= (1ul<<23ul),
    rsFog				= (1ul<<24ul),
    rsRenderRealTime	= (1ul<<25ul),
    rsDrawGrid			= (1ul<<26ul),
    rsDrawSafeRect		= (1ul<<27ul),
    rsMuteSounds		= (1ul<<28ul),
    rsEnvironment		= (1ul<<29ul),
};

#define DEFAULT_CLEARCOLOR 0x00555555

//------------------------------------------------------------------------------
// GPU selection on hybrid-graphics machines.
// D3D9 always enumerates a single adapter there — which physical GPU it maps to
// is decided by the driver before our code runs, so the choice cannot be made at
// device-creation time. Windows' per-application preference is the supported
// knob; it is read at process start and takes effect on the next launch.
//------------------------------------------------------------------------------
enum EGpuPreference
{
	gpuAuto			= 0,	// let Windows decide (usually the integrated chip)
	gpuPowerSaving	= 1,	// integrated
	gpuPerformance	= 2,	// discrete
};

ECORE_API EGpuPreference	GetGpuPreference	();
ECORE_API bool				SetGpuPreference	(EGpuPreference pref);
ECORE_API LPCSTR			GetActiveGpuName	();

// Adapter list and the user's pick. CHW is not exported, so the render layer is
// reached through these instead of touching its statics across the DLL edge.
// Setting a preference only takes effect on the next start: D3D9 cannot move a
// live device to another adapter.
ECORE_API int				GpuAdapterCount		();
ECORE_API LPCSTR			GpuAdapterName		(int index);	// "" when out of range
ECORE_API void				SetPreferredGpu		(LPCSTR description);	// null/"" = system default

#define		REQ_CREATE()	if (!EDevice->bReady)	return;
#define		REQ_DESTROY()	if (EDevice->bReady)	return;

#include "../../../xrCPU_Pipe/xrCPU_Pipe.h"
ENGINE_API extern xrDispatchTable	PSGP;

#include "../../../xrRender/Private/R_Backend_Runtime.h"

