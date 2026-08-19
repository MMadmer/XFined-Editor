//---------------------------------------------------------------------------

#include "stdafx.h"
#include "EDX11Utils.h"
#include "../../Public/xfined_resource.h"
#pragma hdrstop

#include "xr_input.h"
#include "UI_ToolsCustom.h"

#include "UI_Main.h"
#include "EditorPreferences.h"
#include "EditorProject.h"
#include "XFinedMCP.h"
#include "EThumbnailVisual.h"
#include "d3dutils.h"
#include "SoundManager.h"
#include "PSLibrary.h"

#include "UIEditLightAnim.h"
#include "UIImageEditorForm.h"
#include "UISoundEditorForm.h"
#include "UIMinimapEditorForm.h"
#include "UI_CommandPalette.h"
#include "UI_ProgressCenter.h"
#include "..\XrETools\ETools.h"
#include "UILogForm.h"
#include "gamefont.h"
#include "../XrEngine/XR_IOConsole.h"
TUI* 	UI			= 0;

TUI::TUI()
{
	m_FrameWaitTimer = 0;
	m_FrameWakeEvent = ::CreateEventW(NULL, FALSE, FALSE, NULL);
	m_FrameClockFrequency = 1000;
	m_LastFrameStartedAt = 0;
	m_LastFrameIntervalTicks = 0;
	m_FramePacingFrames = 0;
	m_FramePacingWaits = 0;
	m_FramePacingWaitTicks = 0;
	m_LastFramePacingReason = 0;
	m_ProgressOperationDepth = 0;
	m_ProgressCancelable = false;
	m_ProgressImplicitOperation = false;
	m_ProgressDeferredQuit = false;
	m_LastProgressMessagePumpMs = 0;
	m_LastProgressPaintMs = 0;
	LARGE_INTEGER clock_frequency;
	if (::QueryPerformanceFrequency(&clock_frequency))
		m_FrameClockFrequency = u64(clock_frequency.QuadPart);

	// Windows 10's high-resolution timer keeps a 120 Hz cap precise without
	// raising the process-wide timer resolution. Older systems use the regular
	// waitable timer through the fallback below.
	using TCreateWaitableTimerExW = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
	if (HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll"))
		if (TCreateWaitableTimerExW create_timer = reinterpret_cast<TCreateWaitableTimerExW>(
			::GetProcAddress(kernel, "CreateWaitableTimerExW")))
			m_FrameWaitTimer = create_timer(NULL, NULL, 0x00000002, TIMER_MODIFY_STATE | SYNCHRONIZE);
	if (!m_FrameWaitTimer)
		m_FrameWaitTimer = ::CreateWaitableTimerW(NULL, FALSE, NULL);

    m_HConsole = 0;
	m_ProgressOwnsConsole = false;
	UI				= this;
    m_AppClosed = false;
    m_bAppActive 	= false;
	m_bReady 		= false;
    bNeedAbort   	= false;

	m_CurrentRStart.set(0,0,0);
	m_CurrentRDir.set(0,0,0);

	m_Flags.assign	(flResize);

	m_Pivot.set		( 0, 0, 0 );

	m_MouseCaptured = false;
    m_MouseMultiClickCaptured = false;
 	m_SelectionRect = false;
    bMouseInUse		= false;

    m_bHintShowing	= false;
	m_LastHint		= "";
    m_Size.set(1280, 800);
}
//---------------------------------------------------------------------------
TUI::~TUI()
{
	VERIFY(m_ProgressItems.size()==0);
    VERIFY(m_EditorState.size()==0);
	if (m_FrameWaitTimer)
	{
		::CloseHandle(m_FrameWaitTimer);
		m_FrameWaitTimer = 0;
	}
	if (m_FrameWakeEvent)
	{
		::CloseHandle(m_FrameWakeEvent);
		m_FrameWakeEvent = 0;
	}
}

void TUI::OnDeviceCreate()
{
	DU_impl.OnDeviceCreate();

}

void TUI::OnDeviceDestroy()
{
	DU_impl.OnDeviceDestroy();
}

bool TUI::IsModified()
{
	return ExecCommand(COMMAND_CHECK_MODIFIED);
}
//---------------------------------------------------------------------------

void TUI::EnableSelectionRect( bool flag ){
	m_SelectionRect = flag;
	m_SelEnd.x = m_SelStart.x = 0;
	m_SelEnd.y = m_SelStart.y = 0;
}

void TUI::UpdateSelectionRect( const Ivector2& from, const Ivector2& to ){
	m_SelStart.set(from);
	m_SelEnd.set(to);
}

bool  TUI::KeyDown (WORD Key, TShiftState Shift)
{
	if (!m_bReady) return false;
    // project browser owns the keyboard: typing a project name must not feed
    // the engine console or any tool. The Link Game page is just as blocking,
    // so typing a game path must not leak either.
    if (!EditorProject::Active() || !EditorProject::GameLinked()) return true;
    if (Console->bVisible)
    {
        if (Key == 0xC0)
        {
            Console->Hide();
        }
        return true;
    }
   
    if (Key == 0xC0)
    {
        Console->Show();
        return true;
    }
//	m_ShiftState = Shift;
//	Log("Dn  ",Shift.Contains(ssShift)?"1":"0");
	if (EDevice->m_Camera.KeyDown(Key,Shift)) return true;
    return Tools->KeyDown(Key, Shift);
}

bool  TUI::KeyUp   (WORD Key, TShiftState Shift)
{
	if (!m_bReady) return false;
    if (!EditorProject::Active() || !EditorProject::GameLinked()) return true;
//	m_ShiftState = Shift;
	if (EDevice->m_Camera.KeyUp(Key,Shift)) return true;
    return Tools->KeyUp(Key, Shift);
}

bool  TUI::KeyPress(WORD Key, TShiftState Shift)
{
	if (!m_bReady) return false;
    if (!EditorProject::Active() || !EditorProject::GameLinked()) return true;
    return Tools->KeyPress(Key, Shift);
}
//----------------------------------------------------

void TUI::MousePress(TShiftState Shift, int X, int Y)
{
	if (!m_bReady) return;
    if (m_MouseCaptured) return;

    bMouseInUse = true;

    m_ShiftState = Shift;

    // camera activate
    if(!EDevice->m_Camera.MoveStart(m_ShiftState)){
    	if (Tools->Pick(Shift)) return;
        if( !m_MouseCaptured ){
            if(! Tools->HiddenMode() )
            {
                m_CurrentCp = GetRenderMousePosition();
                m_StartCp = m_CurrentCp;
                EDevice->m_Camera.MouseRayFromPoint(m_CurrentRStart, m_CurrentRDir, m_CurrentCp );
                m_StartRStart = m_CurrentRStart;
                m_StartRDir = m_CurrentRDir;
            }
           
            if(Tools->MouseStart(m_ShiftState))
            {
                if(Tools->HiddenMode()) ShowCursor( FALSE );
                m_MouseCaptured = true;
            }

            if (Tools->HiddenMode())
            {
                IR_GetMousePosScreen(m_StartCpH);
                m_DeltaCpH.set(0, 0);
            }
        }
    }
    RedrawScene();
}

void TUI::MouseRelease(TShiftState Shift, int X, int Y)
{
	if (!m_bReady) return;

    m_ShiftState = Shift;

    if( EDevice->m_Camera.IsMoving() ){
        if (EDevice->m_Camera.MoveEnd(m_ShiftState)) bMouseInUse = false;
    }else{
	    bMouseInUse = false;
        if( m_MouseCaptured ){
            if( !Tools->HiddenMode() ){
                m_CurrentCp = GetRenderMousePosition();
                EDevice->m_Camera.MouseRayFromPoint(m_CurrentRStart,m_CurrentRDir,m_CurrentCp );
            }
            bool bIsHiddenMode = Tools->HiddenMode();
            if( Tools->MouseEnd(m_ShiftState) ){
                if(bIsHiddenMode){
                    SetCursorPos(m_StartCpH.x,m_StartCpH.y);
                    ShowCursor( TRUE );
                }
                m_MouseCaptured = false;
            }
        }
    }
    // update tools (change action)
    Tools->OnFrame	();
    RedrawScene		();
}
//----------------------------------------------------
void TUI::MouseMove(TShiftState Shift, int X, int Y)
{
	if (!m_bReady) return;
    m_ShiftState = Shift;
}
//----------------------------------------------------
void TUI::MouseWheel(TShiftState Shift, float steps)
{
	if (!m_bReady) return;
	EDevice->m_Camera.Wheel(Shift, steps);
	RedrawScene();
}
//----------------------------------------------------
void TUI::IR_OnMouseMove(int x, int y)
{
	if (!m_bReady) return;
	// A left drag no tool took is the camera's: press stays with the tools (click to
	// select, grab to transform), and only once the mouse actually moves and nothing
	// captured it does this become navigation.
	if (!EDevice->m_Camera.IsMoving() && !m_MouseCaptured && !m_MouseMultiClickCaptured &&
		(x || y) && (m_ShiftState & ssLeft) &&
		!(m_ShiftState & (ssRight|ssMiddle|ssAlt|ssShift|ssCtrl)))
		EDevice->m_Camera.LeftDragStart(m_ShiftState);
	if (!EDevice->m_Camera.Process(m_ShiftState,x,y))
    {
        if( m_MouseCaptured || m_MouseMultiClickCaptured )
        {
            if( Tools->HiddenMode() )
            {
				m_DeltaCpH.set(x,y);
                if( m_DeltaCpH.x || m_DeltaCpH.y )
                {
                	Tools->MouseMove(m_ShiftState);
                }
            }
            else
            {
                m_CurrentCp = GetRenderMousePosition();
                EDevice->m_Camera.MouseRayFromPoint(m_CurrentRStart,m_CurrentRDir,m_CurrentCp);
                Tools->MouseMove(m_ShiftState);
            }
		    RedrawScene();
        }
    }
    {
        m_CurrentCp = GetRenderMousePosition();
        EDevice->m_Camera.MouseRayFromPoint(m_CurrentRStart, m_CurrentRDir, m_CurrentCp);
    }
    // Out cursor pos
    OutUICursorPos	();
}
//---------------------------------------------------------------------------

void TUI::OnAppActivate()
{
	// The main HWND also loses WM_ACTIVATE to same-process ImGui viewports.
	// Apply the process-wide state only after the complete message batch settles.
	WakeFramePacing();
}
//---------------------------------------------------------------------------

void TUI::OnAppDeactivate()
{
	WakeFramePacing();
}
//---------------------------------------------------------------------------

bool TUI::ShowHint(const AStringVec& SS)
{
	VERIFY(m_bReady);
  /*  if (SS.size()){
        xr_string S=_ListToSequence2(SS);
        if (m_bHintShowing&&(S==m_LastHint)) return true;
        m_LastHint = S;
        m_bHintShowing = true;
        if (!m_pHintWindow){
            m_pHintWindow = xr_new<THintWindow>((TComponent*)0);
            m_pHintWindow->Brush->Color = (TColor)0x0d9F2FF;
        }
        TRect rect = m_pHintWindow->CalcHintRect(320,S,0);
        rect.Left+=m_HintPoint.x;    rect.Top+=m_HintPoint.y;
        rect.Right+=m_HintPoint.x;   rect.Bottom+=m_HintPoint.y;
        m_pHintWindow->ActivateHint(rect,S);
    }else{
    	m_bHintShowing = false;
        m_LastHint = "";
    }*/
    not_implemented();
    return m_bHintShowing;
}
//---------------------------------------------------------------------------

void TUI::HideHint()
{
	VERIFY(m_bReady);
    m_bHintShowing = false;
}
//---------------------------------------------------------------------------

void TUI::ShowHint(const xr_string& s)
{
	VERIFY			(m_bReady);
    GetCursorPos	(&m_HintPoint);
	AStringVec 		SS;
    SS.push_back	(s);
	Tools->OnShowHint(SS);
    if (!ShowHint(SS)) HideHint();
}
//---------------------------------------------------------------------------

void TUI::ShowObjectHint()
{
	/*VERIFY(m_bReady);
    if (!EPrefs->object_flags.is(epoShowHint)){
//    	if (m_bHintShowing) HideHint();
    	return;
    }
    if (EDevice->m_Camera.IsMoving()||m_MouseCaptured) return;
    if (!m_bAppActive) return;

    GetCursorPos(&m_HintPoint);
    TWinControl* ctr = FindVCLWindow(m_HintPoint);
    if (ctr!=m_D3DWindow) return;

	AStringVec SS;
	Tools->OnShowHint(SS);
    if (!ShowHint(SS)&&m_pHintWindow) HideHint();*/
}
//---------------------------------------------------------------------------
void TUI::CheckWindowPos(HWND* form)
{
	/*if (form->Left+form->Width>Screen->Width) 	form->Left	= Screen->Width-form->Width;
	if (form->Top+form->Height>Screen->Height)	form->Top 	= Screen->Height-form->Height;
	if (form->Left<0) 							form->Left	= 0;
	if (form->Top<0) 							form->Top 	= 0;*/
}
//---------------------------------------------------------------------------
#include "igame_persistent.h"
void TUI::PrepareRedraw()
{
	VERIFY(m_bReady);
	if (m_Flags.is(flResize)) 			RealResize();
// set render state
    EDevice->SetRS(D3DRS_TEXTUREFACTOR,	0xffffffff);
    // fog
    u32 fog_color;
	float fog_start, fog_end;
    Tools->GetCurrentFog	(fog_color, fog_start, fog_end);
/*
    if (0==g_pGamePersistent->Environment().GetWeather().size())
    {
        g_pGamePersistent->Environment().CurrentEnv->fog_color.set	(color_get_R(fog_color),color_get_G(fog_color),color_get_B(fog_color));
        g_pGamePersistent->Environment().CurrentEnv->fog_far		= fog_end;
        g_pGamePersistent->Environment().CurrentEnv->fog_near		= fog_start;
    }
*/    
	EDevice->SetRS( D3DRS_FOGCOLOR,		fog_color			);
	EDevice->SetRS( D3DRS_RANGEFOGENABLE,	FALSE				);
	if (HW.Caps.bTableFog)	{
		EDevice->SetRS( D3DRS_FOGTABLEMODE,	D3DFOG_LINEAR 	);
		EDevice->SetRS( D3DRS_FOGVERTEXMODE,	D3DFOG_NONE	 	);
	} else {
		EDevice->SetRS( D3DRS_FOGTABLEMODE,	D3DFOG_NONE	 	);
		EDevice->SetRS( D3DRS_FOGVERTEXMODE,	D3DFOG_LINEAR	);
	}
	EDevice->SetRS( D3DRS_FOGSTART,	*(DWORD *)(&fog_start)	);
	EDevice->SetRS( D3DRS_FOGEND,		*(DWORD *)(&fog_end)	);
    // filter
    for (u32 k=0; k<HW.Caps.raster.dwStages; k++){
        if( psDeviceFlags.is(rsFilterLinear)){
            EDevice->SetSS(k,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
            EDevice->SetSS(k,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
            EDevice->SetSS(k,D3DSAMP_MIPFILTER,D3DTEXF_LINEAR);
        } else {
            EDevice->SetSS(k,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
            EDevice->SetSS(k,D3DSAMP_MINFILTER,D3DTEXF_POINT);
            EDevice->SetSS(k,D3DSAMP_MIPFILTER,D3DTEXF_POINT);
        }
    }
	// ligthing
    if (psDeviceFlags.is(rsLighting)) 	EDevice->SetRS(D3DRS_AMBIENT,0x00000000);
    else                				EDevice->SetRS(D3DRS_AMBIENT,0xFFFFFFFF);

    EDevice->SetRS			(D3DRS_FILLMODE, EDevice->dwFillMode);
    EDevice->SetRS			(D3DRS_SHADEMODE,EDevice->dwShadeMode);

    RCache.set_xform_world	(Fidentity);
}
extern ENGINE_API BOOL g_bRendering;

// -trace: breadcrumbs through the first frames. With -flushlog every line hits
// the disk immediately, so after a hard kill the last breadcrumb names the call
// that died. D3D11 bring-up aid.
namespace
{
	const bool	s_ui_trace		= !!strstr(GetCommandLineA(), "-trace");
	u32			s_ui_trace_left	= 3;

	// -heapcheck: validate the process heap at the same points the breadcrumbs
	// sit on. Deliberately silent on success - it must not allocate, or it would
	// shift the heap the same way -trace does and hide the very bug we are after.
	const bool	s_ui_heapchk	= !!strstr(GetCommandLineA(), "-heapcheck");

	void ui_heapchk(LPCSTR where)
	{
		if (!s_ui_heapchk)								return;
		if (::HeapValidate(::GetProcessHeap(), 0, NULL))	return;

		Msg		("! HEAP CORRUPT detected at: %s", where);
		FlushLog();
	}

	// Breadcrumbs go through the single writer in xrCore. A second local one
	// would open bc.txt again with its own file position and silently overwrite
	// what the other module had written.
	IC void ui_breadcrumb(LPCSTR where)	{ Breadcrumb(where); }
}
#define UI_TRACE(x)	do { ui_breadcrumb(x); if (s_ui_trace && s_ui_trace_left) Msg("~ trace: " x); ui_heapchk(x); } while(0)

void TUI::Redraw()
{
	PrepareRedraw();
    try{
        UI_TRACE("enter");

        if (u32(RTSize.x * EDevice->m_ScreenQuality) != RT->dwWidth || u32(RTSize.y * EDevice->m_ScreenQuality) != RT->dwHeight|| !RT->pSurface)
        {
            GetRenderWidth() = RTSize.x * EDevice->m_ScreenQuality;
            GetRenderHeight() = RTSize.y * EDevice->m_ScreenQuality;
            RT.destroy();
            ZB.destroy();
            RT.create("rt_color", RTSize.x * EDevice->m_ScreenQuality, RTSize.y * EDevice->m_ScreenQuality, HW.Caps.fTarget);
            ZB.create("rt_depth", RTSize.x * EDevice->m_ScreenQuality, RTSize.y * EDevice->m_ScreenQuality, D3DFORMAT::D3DFMT_D24X8);
            m_Flags.set(flRedraw, TRUE);
            EDevice->fASPECT = ((float)RTSize.y) / ((float)RTSize.x);
         
            EDevice->m_fNearer = EDevice->mProject._43;
            EDevice->fWidth_2 = GetRenderWidth() / 2.f;
            EDevice->fHeight_2 = GetRenderHeight() / 2.f;
            
            Device->seqDeviceReset.Process(rp_DeviceReset);
            Device->seqResolutionChanged.Process(rp_ScreenResolutionChanged);
            RCache.set_xform_project(EDevice->mProject);
            RCache.set_xform_world(Fidentity);
        }
        if (!UI->IsPlayInEditor())
		{
			EDevice->mProject.build_projection(deg2rad(EDevice->fFOV), EDevice->fASPECT, EDevice->m_Camera.m_Znear, EDevice->m_Camera.m_Zfar);
        }

        // Thumbnail renders need the scene closed (GetRenderTargetData) and must
        // never run inside the ImGui pass below, which is wrapped in catch(...):
        // an exception escaping mid-render would leave the scene unbalanced and
        // kill the next Begin(). Here nothing is open yet, so it is free.
        UI_TRACE("before thumbnails");
        FlushVisualThumbnailQueue(4);
        UI_TRACE("before Begin");

        if (EDevice->Begin())
        {
            UI_TRACE("after Begin");
            if (psDeviceFlags.is(rsRenderRealTime))
                m_Flags.set(flRedraw, TRUE);
            if (m_Flags.is(flRedraw)||UI->IsPlayInEditor())
            {
               
                m_Flags.set(flRedraw, FALSE);
#if defined(USE_DX10) || defined(USE_DX11)
                // a depth target carries its own DSV, pRT stays null for it
                RCache.set_RT(RT->pRT);
                RCache.set_ZB(ZB->pZRT);
                EDevice->Statistic->RenderDUMP_RT.Begin();
                {
                    Fcolor cc;
                    cc.set(EPrefs ? EPrefs->scene_clear_color : 0x0);
                    UI_TRACE("before clear RT");
                    HW.pContext->ClearRenderTargetView(RT->pRT, &cc.r);
                    UI_TRACE("before clear ZB");
                    HW.pContext->ClearDepthStencilView(ZB->pZRT, D3D_CLEAR_DEPTH, 1.f, 0);
                    UI_TRACE("after clear");
                }
#else
                RCache.set_RT(RT->pRT);
                RCache.set_ZB(ZB->pRT);
                EDevice->Statistic->RenderDUMP_RT.Begin();
                {
                    CHK_DX(HW.pDevice->Clear(0, 0, D3DCLEAR_ZBUFFER | D3DCLEAR_TARGET, EPrefs ? EPrefs->scene_clear_color : 0x0, 1, 0));
                }
#endif
                EDevice->UpdateView();
                EDevice->ResetMaterial();
                UI_TRACE("before RenderEnvironment");

                Tools->RenderEnvironment();
                UI_TRACE("after RenderEnvironment");

                //. temporary reset filter (      )
                for (u32 k = 0; k < HW.Caps.raster.dwStages; k++) {
                    if (psDeviceFlags.is(rsFilterLinear)) {
                        EDevice->SetSS(k, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                        EDevice->SetSS(k, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                        EDevice->SetSS(k, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                    }
                    else {
                        EDevice->SetSS(k, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                        EDevice->SetSS(k, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                        EDevice->SetSS(k, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
                    }
                }

                // draw grid
                if (psDeviceFlags.is(rsDrawGrid)) {
                    UI_TRACE("before DrawGrid");
                    DU_impl.DrawGrid();
                    UI_TRACE("before DrawPivot");
                    DU_impl.DrawPivot(m_Pivot);
                    UI_TRACE("after grid");
                }

                try {
                    UI_TRACE("before Tools->Render");
                    Tools->Render();
                    UI_TRACE("after Tools->Render");
                }
                catch (...) {
                    ELog.DlgMsg(mtError, "Please notify AlexMX!!! Critical error has occured in render routine!!! [Type B]");
                }

                // draw selection rect
                if (m_SelectionRect) 	DU_impl.DrawSelectionRect(m_SelStart, m_SelEnd);

                // draw axis
                UI_TRACE("before DrawAxis");
                DU_impl.DrawAxis(EDevice->m_Camera.GetTransform());
                UI_TRACE("after DrawAxis");


                EDevice->Statistic->RenderDUMP_RT.End();
                EDevice->EStatistic->Show(EDevice->pSystemFont);
                UI->OnStats(EDevice->pSystemFont);
                EDevice->SetRS(D3DRS_FILLMODE, D3DFILL_SOLID);
                UI_TRACE("before font render");
                // DU's own font queues the tool captions (way hints, spawn
                // names, light controls) and nothing ever flushed it - the
                // text silently piled up unseen. Same frame slot as the
                // system font, same shader path.
                DU_impl.OnRender();
                EDevice->pSystemFont->OnRender();
                UI_TRACE("after font render");
                EDevice->SetRS(D3DRS_FILLMODE, EDevice->dwFillMode);
                EDevice->seqRender.Process(rp_Render);
                UI_TRACE("after seqRender");
                if (g_pGamePersistent->OnRenderPPUI_query())
                {
                    g_pGamePersistent->OnRenderPPUI_main();
                }
                RCache.set_RT(HW.pBaseRT);
                RCache.set_ZB(HW.pBaseZB);
            }

            try {
                EDevice->SetRS(D3DRS_FILLMODE, D3DFILL_SOLID);
                g_bRendering = FALSE;
#if defined(USE_DX11)
                // ImGui submits through the raw context, not RCache, and
                // RCache's set_RT is lazy - it only binds inside Render(). So
                // whatever target the scene pass left bound would swallow the
                // whole UI. Bind the backbuffer by hand.
                HW.pContext->OMSetRenderTargets(1, &HW.pBaseRT, HW.pBaseZB);
                D3D11_VIEWPORT ui_vp;
                ui_vp.TopLeftX = 0.f;               ui_vp.TopLeftY = 0.f;
                ui_vp.Width    = float(EDevice->dwRealWidth);
                ui_vp.Height   = float(EDevice->dwRealHeight);
                ui_vp.MinDepth = 0.f;               ui_vp.MaxDepth = 1.f;
                HW.pContext->RSSetViewports(1, &ui_vp);
#endif
                UI_TRACE("before imgui Draw");
                Draw();
                UI_TRACE("after imgui Draw");
                EDevice->SetRS(D3DRS_FILLMODE, EDevice->dwFillMode);
                // end draw
                EDevice->End();
                UI_TRACE("after End");
                if (s_ui_trace_left)	--s_ui_trace_left;
            }
            catch (...) {
                ELog.DlgMsg(mtError, "Please notify AlexMX!!! Critical error has occured in render routine!!! [Type C]");
            }

        }
    }catch(...){
    	ELog.DlgMsg(mtError, "Please notify AlexMX!!! Critical error has occured in render routine!!! [Type A]");
//		_clear87();
//		FPU::m24r();
//    	ELog.DlgMsg(mtError, "Critical error has occured in render routine.\nEditor may work incorrectly.");
        EDevice->End();
//		EDevice->Resize(m_D3DWindow->Width,m_D3DWindow->Height);
    }

	OutInfo();
}
//---------------------------------------------------------------------------
void TUI::RealResize()
{
    m_Flags.set			(flResize,FALSE);
    if(m_Size.x&& m_Size.y)
    EDevice->Resize(m_Size.x, m_Size.y,m_Size_Maximize);
    ExecCommand			(COMMAND_UPDATE_PROPERTIES);
}
void TUI::RealUpdateScene()
{
    Tools->UpdateProperties	(false);
    m_Flags.set			(flUpdateScene,FALSE);
}
void TUI::RealRedrawScene()
{

    Redraw				();         
}
void TUI::OnFrame()
{
	EDevice->FrameMove	();
    SndLib->OnFrame		();
    // tools on frame
    if (m_Flags.is(flUpdateScene)) RealUpdateScene();
    Tools->OnFrame		();

	// show hint
    ShowObjectHint		();
#if 0
	// check mail
    CheckMailslot		();
#endif
    // Progress
    ProgressDraw		();
}

namespace
{
constexpr u32 kBackgroundPollMs = 50;

bool IsEditorProcessForeground()
{
	const HWND foreground = ::GetForegroundWindow();
	if (!foreground)
		return false;

	DWORD process_id = 0;
	::GetWindowThreadProcessId(foreground, &process_id);
	return process_id == ::GetCurrentProcessId();
}
}

void TUI::SyncAppActivation()
{
	const bool active = IsEditorProcessForeground();
	EDevice->b_is_Active = active ? TRUE : FALSE;
	if (active == m_bAppActive)
		return;

	m_bAppActive = active;
	// A drag that was running when the window lost focus never sees its button come
	// up, so m_bMoving stayed true with the old buttons still recorded - and the next
	// right-click resumed that drag instead of starting a look. Alt+Tab is the common
	// way in, which is why it came back after every switch away and back.
	if (!active)
	{
		m_ShiftState = 0;
		// only when one is actually running: MoveEnd restores the cursor, and calling
		// it unpaired leaks a ShowCursor count
		if (EDevice->m_Camera.IsMoving()) EDevice->m_Camera.MoveEnd(0);
	}
	if (!m_bReady)
		return;

	if (pInput)
	{
		m_ShiftState = ssNone;
		if (active)
		{
			pInput->OnAppActivate();
			EDevice->seqAppActivate.Process(rp_AppActivate);
		}
		else
		{
			pInput->OnAppDeactivate();
			EDevice->seqAppDeactivate.Process(rp_AppDeactivate);
		}
	}
	if (!active)
		HideHint();
}

namespace
{
enum EFramePacingReason
{
	FramePacingStartup,
	FramePacingIdleDeadline,
	FramePacingBackgroundDeadline,
	FramePacingMessage,
	FramePacingMcpRequest,
	FramePacingExplicitWake,
	FramePacingExplicitWork,
	FramePacingPlayInEditor,
	FramePacingRealtime,
	FramePacingWaitFailed,
};

LPCSTR FramePacingReasonName(u32 reason)
{
	switch (reason)
	{
	case FramePacingIdleDeadline:			return "idle_deadline";
	case FramePacingBackgroundDeadline:	return "background_deadline";
	case FramePacingMessage:				return "window_message";
	case FramePacingMcpRequest:			return "mcp_request";
	case FramePacingExplicitWake:			return "explicit_wake";
	case FramePacingExplicitWork:			return "explicit_work";
	case FramePacingPlayInEditor:			return "play_in_editor";
	case FramePacingRealtime:				return "realtime_render";
	case FramePacingWaitFailed:			return "wait_failed";
	default:							return "startup";
	}
}

void PumpEditorMessages()
{
	MSG msg;
	while (::PeekMessageW(&msg, NULL, 0U, 0U, PM_REMOVE))
	{
		::TranslateMessage(&msg);
		::DispatchMessageW(&msg);
		if (WM_QUIT == msg.message && UI)
			UI->Quit();
	}
}
}

void TUI::WaitForFramePacing()
{
	const bool app_active = IsEditorProcessForeground();
	const bool explicit_work = m_Flags.is(flUpdateScene) || m_Flags.is(flNeedQuit) ||
		m_Flags.is(flResetUI) || (app_active && (m_Flags.is(flRedraw) || m_Flags.is(flResize)));
	if (explicit_work)
	{
		if (m_FrameWakeEvent)
			::WaitForSingleObject(m_FrameWakeEvent, 0);
		m_LastFramePacingReason = FramePacingExplicitWork;
		return;
	}
	if (IsPlayInEditor())
	{
		m_LastFramePacingReason = FramePacingPlayInEditor;
		return;
	}
	if (psDeviceFlags.is(rsRenderRealTime))
	{
		m_LastFramePacingReason = FramePacingRealtime;
		return;
	}

	LARGE_INTEGER now;
	::QueryPerformanceCounter(&now);
	u64 wait_ticks = 0;
	const bool background = !app_active;
	if (background)
	{
		wait_ticks = (m_FrameClockFrequency * kBackgroundPollMs + 999) / 1000;
	}
	else
	{
		const u32 limit = EPrefs
			? clampr(EPrefs->active_idle_fps, kEditorIdleFpsMinimum, kEditorIdleFpsMaximum)
			: kEditorIdleFpsDefault;
		const u64 interval = (m_FrameClockFrequency + limit - 1) / limit;
		if (m_LastFrameStartedAt)
		{
			const u64 elapsed = u64(now.QuadPart) - m_LastFrameStartedAt;
			if (elapsed < interval)
				wait_ticks = interval - elapsed;
		}
		if (!wait_ticks)
		{
			m_LastFramePacingReason = FramePacingIdleDeadline;
			return;
		}
	}

	HANDLE handles[3] = {};
	DWORD handle_count = 0;
	const DWORD wake_index = m_FrameWakeEvent ? handle_count : DWORD(-1);
	if (m_FrameWakeEvent)
		handles[handle_count++] = m_FrameWakeEvent;
	const HANDLE mcp_event = XFinedMCP::WakeEvent();
	const DWORD mcp_index = mcp_event ? handle_count : DWORD(-1);
	if (mcp_event)
		handles[handle_count++] = mcp_event;

	DWORD timer_index = DWORD(-1);
	DWORD timeout = INFINITE;
	bool timer_armed = false;
	if (m_FrameWaitTimer)
	{
		const u64 hundred_ns = _max<u64>(1,
			(wait_ticks * 10000000ull + m_FrameClockFrequency - 1) / m_FrameClockFrequency);
		LARGE_INTEGER due;
		due.QuadPart = -LONGLONG(hundred_ns);
		if (::SetWaitableTimer(m_FrameWaitTimer, &due, 0, NULL, NULL, FALSE))
		{
			timer_index = handle_count;
			handles[handle_count++] = m_FrameWaitTimer;
			timer_armed = true;
		}
	}
	if (!timer_armed)
		timeout = DWORD(_max<u64>(1,
			(wait_ticks * 1000ull + m_FrameClockFrequency - 1) / m_FrameClockFrequency));

	LARGE_INTEGER wait_started;
	::QueryPerformanceCounter(&wait_started);
	const DWORD result = ::MsgWaitForMultipleObjectsEx(handle_count, handles, timeout,
		QS_ALLINPUT, MWMO_INPUTAVAILABLE);
	LARGE_INTEGER wait_finished;
	::QueryPerformanceCounter(&wait_finished);
	++m_FramePacingWaits;
	m_FramePacingWaitTicks += u64(wait_finished.QuadPart - wait_started.QuadPart);

	if (timer_armed)
	{
		::CancelWaitableTimer(m_FrameWaitTimer);
		::WaitForSingleObject(m_FrameWaitTimer, 0);
	}

	if (wake_index != DWORD(-1) && result == WAIT_OBJECT_0 + wake_index)
		m_LastFramePacingReason = FramePacingExplicitWake;
	else if (mcp_index != DWORD(-1) && result == WAIT_OBJECT_0 + mcp_index)
		m_LastFramePacingReason = FramePacingMcpRequest;
	else if (timer_index != DWORD(-1) && result == WAIT_OBJECT_0 + timer_index)
		m_LastFramePacingReason = background ? FramePacingBackgroundDeadline : FramePacingIdleDeadline;
	else if (result == WAIT_OBJECT_0 + handle_count)
		m_LastFramePacingReason = FramePacingMessage;
	else if (WAIT_TIMEOUT == result)
		m_LastFramePacingReason = background ? FramePacingBackgroundDeadline : FramePacingIdleDeadline;
	else
		m_LastFramePacingReason = FramePacingWaitFailed;
}

void TUI::GetFramePacingStats(SFramePacingStats& result)
{
	result.active_idle_fps = EPrefs
		? clampr(EPrefs->active_idle_fps, kEditorIdleFpsMinimum, kEditorIdleFpsMaximum)
		: kEditorIdleFpsDefault;
	result.background_poll_ms = kBackgroundPollMs;
	result.measured_frame_ms = m_LastFrameIntervalTicks
		? float(double(m_LastFrameIntervalTicks) * 1000.0 / double(m_FrameClockFrequency))
		: 0.f;
	result.measured_fps = result.measured_frame_ms > EPS_S ? 1000.f / result.measured_frame_ms : 0.f;
	result.last_wait_reason = FramePacingReasonName(m_LastFramePacingReason);
	result.app_active = IsEditorProcessForeground();
	result.play_in_editor = IsPlayInEditor();
	result.realtime_render = psDeviceFlags.is(rsRenderRealTime);
	result.redraw_pending = m_Flags.is(flRedraw);
	result.idle_cap_active = result.app_active && !result.play_in_editor &&
		!result.realtime_render && !result.redraw_pending;
	result.frames = m_FramePacingFrames;
	result.waits = m_FramePacingWaits;
	result.waited_us = m_FrameClockFrequency
		? u64(double(m_FramePacingWaitTicks) * 1000000.0 / double(m_FrameClockFrequency))
		: 0;
}

bool TUI::ShouldDeferCommand(u32 command) const
{
	switch (command)
	{
	case COMMAND_LOAD:
	case COMMAND_SAVE:
	case COMMAND_SAVE_BACKUP:
	case COMMAND_CHECK_TEXTURES:
	case COMMAND_REFRESH_TEXTURES:
	case COMMAND_RELOAD_TEXTURES:
	case COMMAND_UNLOAD_TEXTURES:
	case COMMAND_EVICT_OBJECTS:
	case COMMAND_EVICT_TEXTURES:
	case COMMAND_CREATE_SOUND_LIB:
	case COMMAND_SYNC_SOUNDS:
		return true;
	default:
		return false;
	}
}

bool TUI::DeferCommand(u32 command, CCommandVar p1, CCommandVar p2)
{
	m_DeferredCommands.push_back({ command, p1, p2 });
	WakeFramePacing();
	return true;
}

bool TUI::DeferUIWork(TDeferredUIWork work)
{
	if (!work)
		return false;
	m_DeferredUIWork.push_back(work);
	WakeFramePacing();
	return true;
}

void TUI::ProcessDeferredWork()
{
	if (ProgressOperationActive())
		return;

	if (!m_DeferredCommands.empty())
	{
		const SDeferredEditorCommand command = m_DeferredCommands.front();
		m_DeferredCommands.erase(m_DeferredCommands.begin());
		ExecCommand(command.command, command.p1, command.p2);
		return;
	}

	if (!m_DeferredUIWork.empty())
	{
		const TDeferredUIWork work = m_DeferredUIWork.front();
		m_DeferredUIWork.erase(m_DeferredUIWork.begin());
		work();
	}
}

bool TUI::Idle()         
{
	VERIFY(m_bReady);
	// Drain first so commands generated by input can mark explicit work. The
	// interruptible wait then reacts to either the next message or an MCP request.
	PumpEditorMessages();
	SyncAppActivation();
	if (m_Flags.is(flResetUI)) RealResetUI();
	WaitForFramePacing();
	PumpEditorMessages();
	SyncAppActivation();
	if (m_Flags.is(flResetUI)) RealResetUI();

	LARGE_INTEGER frame_started;
	::QueryPerformanceCounter(&frame_started);
	if (m_LastFrameStartedAt)
		m_LastFrameIntervalTicks = u64(frame_started.QuadPart) - m_LastFrameStartedAt;
	m_LastFrameStartedAt = u64(frame_started.QuadPart);
	++m_FramePacingFrames;

    OnFrame			();
    // MCP requests must be served even when the window is inactive — the
    // draw path (and the Pump call inside it) stops for background windows
    XFinedMCP::Pump	();
	SyncAppActivation();
	// A background editor does not draw: repainting a window nobody is looking at
	// would burn a core for nothing. An armed capture is the one exception - there
	// is nothing to mirror without a presented frame, so every screenshot asked for
	// over MCP failed for as long as the editor was not the foreground window, which
	// is every time anything but a human asks. The exception ends with the frame.
	bool draw = m_bAppActive;
#if defined(USE_DX11)
	if (!draw)	draw = DX11FrameCaptureWants();
#endif
	if (draw && !m_Flags.is(flNeedQuit) && !m_AppClosed)
		RealRedrawScene();

    {
        for (u32 pit = 0; pit < Device->seqParallel.size(); pit++)
            Device->seqParallel[pit]();
        Device->seqParallel.clear_not_free();
        Device->seqFrameMT.Process(rp_Frame);
    }
	ProcessDeferredWork();
	// Notifications produced during this iteration are represented by their
	// flags; draining the event avoids a stale wake on the next iteration.
	if (m_FrameWakeEvent)
		::WaitForSingleObject(m_FrameWakeEvent, 0);
    // test quit
    if (m_Flags.is(flNeedQuit))	RealQuit();
    return !m_AppClosed;
}
//---------------------------------------------------------------------------
void ResetActionToSelect()
{
    ExecCommand(COMMAND_CHANGE_ACTION, etaSelect);
}
//---------------------------------------------------------------------------

#define MIN_PANEL_HEIGHT 15


bool TUI::OnCreate()
{
// create base class
	EDevice->InitTimer();

  //  m_D3DWindow 	= w;
  //  m_D3DPanel		= p;
    EDevice->Initialize();
	// Creation
	ETOOLS::ray_options	(CDB::OPT_ONLYNEAREST | CDB::OPT_CULL);

    pInput			= xr_new<CInput>(FALSE, all_device_key);

    Console = xr_new<CConsole>();
    Console->Initialize();

    UI->IR_Capture	();

    m_bReady		= true;

#if 0
    if (!CreateMailslot()) {
        ELog.DlgMsg(mtError, "Can't create mail slot.\nIt's possible two Editors started.");
        return 		false;
    }
#endif
    string_path log_path;
    if (!FS.exist(log_path,_temp_,""))
    {
        VerifyPath(log_path);
    }
    if (!FS.path_exist(_local_root_)){
    	ELog.DlgMsg	(mtError,"Undefined Editor local directory.");
        return 		false;
    }

	BeginEState		(esEditScene);
    GetRenderWidth() = 128;
    GetRenderHeight() = 128;
    RTSize.set(GetRenderWidth(), GetRenderHeight());
    EDevice->fASPECT = (float)RTSize.x / (float)RTSize.y;
    EDevice->mProject.build_projection(deg2rad(EDevice->fFOV), EDevice->fASPECT, EDevice->m_Camera.m_Znear, EDevice->m_Camera.m_Zfar);
    EDevice->m_fNearer = EDevice->mProject._43;


    RCache.set_xform_project(EDevice->mProject);
    RCache.set_xform_world(Fidentity);
    RT.create("rt_color", RTSize .x*EDevice->m_ScreenQuality, RTSize.y * EDevice->m_ScreenQuality, HW.Caps.fTarget);
    ZB.create("rt_depth", RTSize.x * EDevice->m_ScreenQuality, RTSize.y* EDevice->m_ScreenQuality, D3DFORMAT::D3DFMT_D24X8);

    return true;
}

void TUI::OnDestroy()
{
    Console->Destroy();
    xr_delete(Console);
    RT.destroy();
    ZB.destroy();

	VERIFY(m_bReady);
	m_bReady		= false;
	UI->IR_Release	();
    xr_delete		(pInput);
    EndEState		();

    EDevice->ShutDown();    
}

// A console allocated by the editor is a window of this process like any other:
// untouched it wears the stock console icon and an empty title in the taskbar.
static void XFinedBrandConsole()
{
	::SetConsoleTitleA("XFined Editor");
	const HWND con = ::GetConsoleWindow();
	if (!con) return;
	const HINSTANCE self = ::GetModuleHandle(NULL);
	HICON big = (HICON)::LoadImageA(self, MAKEINTRESOURCEA(IDI_XFINED_EDITOR), IMAGE_ICON,
		::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_SHARED);
	HICON small_ = (HICON)::LoadImageA(self, MAKEINTRESOURCEA(IDI_XFINED_EDITOR), IMAGE_ICON,
		::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_SHARED);
	if (big)	::SendMessageA(con, WM_SETICON, ICON_BIG, (LPARAM)big);
	if (small_)	::SendMessageA(con, WM_SETICON, ICON_SMALL, (LPARAM)small_);
}

void TUI::BeginProgressOperation(bool cancelable)
{
	if (!m_ProgressOperationDepth)
	{
		ResetBreak();
		m_ProgressCancelable = cancelable;
		m_ProgressImplicitOperation = false;
		m_ProgressOwnsConsole = !m_HConsole;
		m_LastProgressMessagePumpMs = 0;
		m_LastProgressPaintMs = 0;
		SetProgressOnlyInput(true);
	}
	++m_ProgressOperationDepth;
}

void TUI::EndProgressOperation()
{
	VERIFY(m_ProgressOperationDepth);
	if (--m_ProgressOperationDepth)
		return;

	SetProgressOnlyInput(false);
	ResetBreak();
	m_ProgressCancelable = false;
	m_ProgressImplicitOperation = false;
	if (m_ProgressOwnsConsole)
		CloseConsole();
	m_ProgressOwnsConsole = false;
	ReplayDeferredWindowMessages();
}

bool TUI::RequestProgressCancel()
{
	if (!ProgressCancelable() || bNeedAbort)
		return false;
	bNeedAbort = true;
	return true;
}

void TUI::PumpProgressMessages()
{
	MSG msg;
	while (::PeekMessageW(&msg, NULL, 0U, 0U, PM_REMOVE))
	{
		if (WM_QUIT == msg.message)
		{
			m_ProgressDeferredQuit = true;
			continue;
		}

		const bool main_window = msg.hwnd == EDevice->m_hWnd;
		const bool close_request = WM_CLOSE == msg.message ||
			(WM_SYSCOMMAND == msg.message && SC_CLOSE == (msg.wParam & 0xfff0)) ||
			(WM_SYSKEYDOWN == msg.message && VK_F4 == msg.wParam);
		if (close_request)
		{
			if (main_window)
			{
				const bool already_deferred = std::find_if(m_DeferredWindowMessages.begin(),
					m_DeferredWindowMessages.end(), [](const SDeferredWindowMessage& deferred)
					{
						return WM_CLOSE == deferred.message;
					}) != m_DeferredWindowMessages.end();
				if (!already_deferred)
					m_DeferredWindowMessages.push_back({ msg.hwnd, WM_CLOSE, 0, 0 });
			}
			continue;
		}

		if (WM_SIZE == msg.message)
		{
			if (main_window)
			{
				auto deferred = std::find_if(m_DeferredWindowMessages.begin(),
					m_DeferredWindowMessages.end(), [](const SDeferredWindowMessage& item)
					{
						return WM_SIZE == item.message;
					});
				if (deferred == m_DeferredWindowMessages.end())
					m_DeferredWindowMessages.push_back({ msg.hwnd, msg.message, msg.wParam, msg.lParam });
				else
				{
					deferred->wparam = msg.wParam;
					deferred->lparam = msg.lParam;
				}
			}
			continue;
		}

		switch (msg.message)
		{
		case WM_SIZING:
		case WM_ENTERSIZEMOVE:
		case WM_EXITSIZEMOVE:
		case WM_NCLBUTTONDOWN:
		case WM_NCLBUTTONDBLCLK:
		case WM_WINDOWPOSCHANGING:
		case WM_WINDOWPOSCHANGED:
		case WM_DPICHANGED:
		case WM_SYSCOMMAND:
			continue;
		default:
			break;
		}

		::TranslateMessage(&msg);
		::DispatchMessageW(&msg);
	}
}

void TUI::ReplayDeferredWindowMessages()
{
	for (const SDeferredWindowMessage& deferred : m_DeferredWindowMessages)
	{
		if (deferred.window == EDevice->m_hWnd && ::IsWindow(deferred.window))
			::PostMessageW(deferred.window, deferred.message, deferred.wparam, deferred.lparam);
	}
	m_DeferredWindowMessages.clear();
	if (m_ProgressDeferredQuit)
	{
		m_ProgressDeferredQuit = false;
		::PostQuitMessage(0);
	}
}

void TUI::DrawProgressFrame()
{
	if (!m_bReady || m_ProgressItems.empty() || InUIPass() || !EDevice->Begin())
		return;

	try
	{
		EDevice->SetRS(D3DRS_FILLMODE, D3DFILL_SOLID);
		g_bRendering = FALSE;
#if defined(USE_DX11)
		HW.pContext->OMSetRenderTargets(1, &HW.pBaseRT, HW.pBaseZB);
		D3D11_VIEWPORT viewport = {};
		viewport.Width = float(EDevice->dwRealWidth);
		viewport.Height = float(EDevice->dwRealHeight);
		viewport.MaxDepth = 1.f;
		HW.pContext->RSSetViewports(1, &viewport);
#endif
		DrawProgressOnly();
	}
	catch (...)
	{
		ELog.Msg(mtError, "Progress Center frame failed; the active operation continues.");
	}
	EDevice->SetRS(D3DRS_FILLMODE, EDevice->dwFillMode);
	EDevice->End();
}

void TUI::ProgressCheckpoint()
{
	if (!ProgressOperationActive() || InUIPass())
		return;

	const u64 now = ::GetTickCount64();
	if (!m_LastProgressMessagePumpMs || now - m_LastProgressMessagePumpMs >= 50)
	{
		m_LastProgressMessagePumpMs = now;
		PumpProgressMessages();
		SyncAppActivation();
		XFinedMCP::PumpProgressRequests();
	}
	if (!m_ProgressItems.empty() && m_bAppActive &&
		(!m_LastProgressPaintMs || now - m_LastProgressPaintMs >= 100))
	{
		m_LastProgressPaintMs = now;
		DrawProgressFrame();
	}
}

SPBItem* TUI::ProgressStart(float max_val, LPCSTR text)
{
	VERIFY(m_bReady);
	if (!ProgressOperationActive())
	{
		BeginProgressOperation(false);
		m_ProgressImplicitOperation = true;
	}
	if (m_ProgressImplicitOperation && m_ProgressItems.empty() && m_ProgressOwnsConsole && !m_HConsole)
		ShowConsole();

	SPBItem* item = xr_new<SPBItem>(text, "", max_val);
	m_ProgressItems.push_back(item);
	ELog.Msg(mtInformation, "%s", item->text.c_str());
	ProgressDraw();
	return item;
}

void TUI::ProgressEnd(SPBItem*& pbi)
{
	VERIFY(m_bReady);
	if (!pbi)
		return;

	PBVecIt it = std::find(m_ProgressItems.begin(), m_ProgressItems.end(), pbi);
	VERIFY(it != m_ProgressItems.end());
	const u64 elapsed_ms = ::GetTickCount64() - pbi->started_at_ms;
	if (elapsed_ms >= 500)
	{
		xr_string elapsed;
		UIProgressCenter::FormatElapsed(elapsed_ms, elapsed);
		ELog.Msg(mtInformation, "%s finished in %s.", pbi->text.c_str(), elapsed.c_str());
	}
	m_ProgressItems.erase(it);
	xr_delete(pbi);
	ProgressDraw();
	if (m_ProgressItems.empty() && m_ProgressImplicitOperation)
		EndProgressOperation();
}

void TUI::ProgressDraw()
{
	if (!m_ProgressItems.empty())
		RedrawScene();
}

void TUI::GetProgressSnapshot(SProgressTaskInfoVec& result) const
{
	result.clear();
	result.reserve(m_ProgressItems.size());
	const u64 now = ::GetTickCount64();
	for (u32 i = 0; i < m_ProgressItems.size(); ++i)
	{
		const SPBItem& item = *m_ProgressItems[i];
		SProgressTaskInfo task;
		task.text = item.text.c_str();
		task.detail = item.info.c_str();
		task.current = item.progress;
		task.total = item.max;
		task.determinate = item.max > EPS_S;
		task.fraction = task.determinate ? clampr(item.progress / item.max, 0.f, 1.f) : 0.f;
		task.elapsed_ms = now - item.started_at_ms;
		task.depth = i;
		task.cancelable = ProgressCancelable();
		task.cancel_requested = task.cancelable && bNeedAbort;
		result.push_back(std::move(task));
	}
}

void TUI::ShowConsole()
{
	if (!m_HConsole)
	{
		AllocConsole();
		m_HConsole = GetStdHandle(STD_OUTPUT_HANDLE);
		SetConsoleTextAttribute(m_HConsole, 15);
		XFinedBrandConsole();
	}
}

void TUI::WriteConsole(TMsgDlgType mt, const char* txt)
{
    if (m_HConsole)
	{
		switch (mt)
		{
		case mtError:
			SetConsoleTextAttribute(m_HConsole, 12);
            break;
		case mtInformation:
			SetConsoleTextAttribute(m_HConsole, 11);
            break;
		case mtConfirmation:
			SetConsoleTextAttribute(m_HConsole, 14);
            break;
        default:
            SetConsoleTextAttribute(m_HConsole,15);
            break;
        }

		DWORD  dw;
		::WriteConsole(m_HConsole, txt, xr_strlen(txt), &dw, NULL);
		::WriteConsole(m_HConsole, "\r\n", 2, &dw, NULL);
    }
}

void TUI::CloseConsole()
{
	if (m_ProgressItems.size() == 0)
	{
		FreeConsole();
		m_HConsole = 0;
	}
}

void TUI::OnDrawUI()
{
    UIKeyPressForm::Update(EDevice->fTimeGlobal);
    UIEditLightAnim::Update();
    UIImageEditorForm::Update();
    UISoundEditorForm::Update();
    UIMinimapEditorForm::Update();
    UILogForm::Update();
    EDevice->seqDrawUI.Process(rp_DrawUI);
	CommandPalette::Draw();
	UIProgressCenter::Draw(*this);
}

void TUI::OnDrawProgressUI()
{
	UIProgressCenter::Draw(*this);
}

void TUI::RealResetUI()
{
    m_Flags.set(flResetUI, FALSE);
    string_path 		ini_path;
    if (FS.exist(ini_path, "$server_data_root$", UI->EditorName(), "_imgui_default.ini"))
    {
        UI->Resize(1280, 800);
        ImGui::LoadIniSettingsFromDisk(ini_path);
    }
}

void TUI::OnStats(CGameFont* font)
{
}

SPBItem::SPBItem(LPCSTR txt, LPCSTR inf, float mx) :
	text(txt ? txt : ""), info(inf ? inf : ""), max(mx), progress(0.f),
	started_at_ms(::GetTickCount64()), last_publish_ms(0), last_log_ms(0),
	last_publish_percent(-1), last_log_percent(-1)
{
}

void SPBItem::GetInfo			(xr_string& txt, float& p, float& m)
{
    if (info.size())txt.sprintf("%s (%s)",text.c_str(),info.c_str());
    else			txt.sprintf("%s",text.c_str());
    p				= progress;
    m				= max;
}  
void SPBItem::Inc				(LPCSTR info, bool bWarn)
{
	if (info && info[0])
		this->info = info;
	progress += 1.f;
	Publish(bWarn, bWarn);
}
void SPBItem::Update			(float val)
{
    progress					= val;
	Publish(false, false);
}
void SPBItem::Info				(LPCSTR text, bool bWarn)
{
	if (text&&text[0]){
    	info					= text;
		Publish(bWarn, bWarn);
    }
}

s32 SPBItem::Percent() const
{
	if (max <= EPS_S)
		return 0;
	return iFloor(clampr(progress / max, 0.f, 1.f) * 100.f);
}

void SPBItem::Publish(bool force, bool warning)
{
	const u64 now = ::GetTickCount64();
	const s32 percent = Percent();
	const bool finished = max > EPS_S && progress >= max && last_publish_percent != 100;
	const bool percentage_changed = percent != last_publish_percent;
	const bool draw_due = force || finished || last_publish_ms == 0 ||
		(percentage_changed && now - last_publish_ms >= 50);
	if (draw_due)
	{
		last_publish_ms = now;
		last_publish_percent = percent;
		if (UI)
			UI->ProgressDraw();
	}

	xr_string label;
	float current = 0.f;
	float total = 0.f;
	GetInfo(label, current, total);
	xr_string message;
	if (max > EPS_S)
		message.sprintf("[%d%%] %s", percent, label.c_str());
	else
		message = label;
	if (warning)
	{
		ELog.Msg(mtError, "%s", message.c_str());
		last_log_ms = now;
		last_log_percent = percent;
		last_logged_info = info;
		return;
	}

	const bool detail_changed = last_logged_info != info;
	const bool first_detail = detail_changed && !last_logged_info.size() && info.size();
	const bool progress_due = percentage_changed && percent - last_log_percent >= 10 &&
		(last_log_ms == 0 || now - last_log_ms >= 500);
	const bool time_due = percentage_changed && last_log_ms && now - last_log_ms >= 2000;
	const bool log_due = first_detail || finished || progress_due || time_due;
	if (log_due)
	{
		ELog.Msg(mtInformation, "%s", message.c_str());
		last_log_ms = now;
		last_log_percent = percent;
		last_logged_info = info;
	}
}

SProgressOperation::SProgressOperation(TUI& ui, bool cancelable) : m_UI(&ui)
{
	m_UI->BeginProgressOperation(cancelable);
}

SProgressOperation::~SProgressOperation()
{
	if (m_UI)
		m_UI->EndProgressOperation();
}

