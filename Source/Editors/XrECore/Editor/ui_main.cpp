//---------------------------------------------------------------------------

#include "stdafx.h"
#include "../../Public/xfined_resource.h"
#pragma hdrstop

#include "xr_input.h"
#include "UI_ToolsCustom.h"

#include "UI_Main.h"
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
#include "..\XrETools\ETools.h"
#include "UILogForm.h"
#include "gamefont.h"
#include "../XrEngine/XR_IOConsole.h"
TUI* 	UI			= 0;

TUI::TUI()
{
    m_HConsole = 0;
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
    m_bAppActive = true;
    if (!m_bReady)return;
	if (pInput){
        m_ShiftState = ssNone;
     	pInput->OnAppActivate();
        EDevice->seqAppActivate.Process	(rp_AppActivate);
    }
}
//---------------------------------------------------------------------------

void TUI::OnAppDeactivate()
{
    m_bAppActive = false;
    if (!m_bReady)return;
	if (pInput){
		pInput->OnAppDeactivate();
        m_ShiftState = ssNone;
        EDevice->seqAppDeactivate.Process(rp_AppDeactivate);
    }
    HideHint();
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
	ResetBreak			();
#if 0
	// check mail
    CheckMailslot		();
#endif
    // Progress
    ProgressDraw		();
}
bool TUI::Idle()         
{
	VERIFY(m_bReady);
   // EDevice->b_is_Active  = Application->Active;
	// input
    MSG msg;
    do
    {
        ZeroMemory(&msg, sizeof(msg));
        if (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
            {
                UI->Quit();
            }
            continue;
        }

    } while (msg.message);
    if (m_Flags.is(flResetUI))RealResetUI();
    Sleep(1);

    OnFrame			();
    // MCP requests must be served even when the window is inactive — the
    // draw path (and the Pump call inside it) stops for background windows
    XFinedMCP::Pump	();
    if (m_bAppActive && !m_Flags.is(flNeedQuit) && !m_AppClosed)
    RealRedrawScene();

    {
        for (u32 pit = 0; pit < Device->seqParallel.size(); pit++)
            Device->seqParallel[pit]();
        Device->seqParallel.clear_not_free();
        Device->seqFrameMT.Process(rp_Frame);
    }
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

SPBItem* TUI::ProgressStart		(float max_val, LPCSTR text)
{
	VERIFY(m_bReady);
	SPBItem* item 				= xr_new<SPBItem>(text,"",max_val);
    m_ProgressItems.push_back	(item);
    ELog.Msg					(mtInformation,text);
    ProgressDraw				();
    if (!m_HConsole)
    {
        AllocConsole();
        m_HConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        XFinedBrandConsole();
    }
	return item;
}
void TUI::ProgressEnd			(SPBItem*& pbi)
{
	VERIFY(m_bReady);
    if (pbi){
        PBVecIt it=std::find(m_ProgressItems.begin(),m_ProgressItems.end(),pbi); VERIFY(it!=m_ProgressItems.end());
        m_ProgressItems.erase	(it);
        xr_delete				(pbi);
        ProgressDraw			();
        if (m_ProgressItems.size() == 0)
        {
            FreeConsole();
            m_HConsole = 0;
        }
    }
}

void TUI::ProgressDraw()
{
    SPBItem* pbi = UI->ProgressLast();
    if (pbi) 
    {
        xr_string txt;
        float 		p, m;
        pbi->GetInfo(txt, p, m);
        // progress
        int val = fis_zero(m) ? 0 : (int)((p / m) * 100);
        string2048 out;
        xr_sprintf(out,"[%d%%]%s\r\n", val, txt.c_str());
        DWORD  dw;
        SetConsoleTextAttribute(m_HConsole, 10);
        ::WriteConsole(m_HConsole, out, xr_strlen(out), &dw, NULL);
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

void SPBItem::GetInfo			(xr_string& txt, float& p, float& m)
{
    if (info.size())txt.sprintf("%s (%s)",text.c_str(),info.c_str());
    else			txt.sprintf("%s",text.c_str());
    p				= progress;
    m				= max;
}  
void SPBItem::Inc				(LPCSTR info, bool bWarn)
{
    Info						(info,bWarn);
    Update						(progress+1.f);
}
void SPBItem::Update			(float val)
{
    progress					= val;
    UI->ProgressDraw			();
}
void SPBItem::Info				(LPCSTR text, bool bWarn)
{
	if (text&&text[0]){
    	info					= text;
        xr_string 				txt;
        float 					p,m;
        GetInfo					(txt,p,m);
	    ELog.Msg				(bWarn?mtError:mtInformation,txt.c_str());
	    UI->ProgressDraw		();
    }
}

