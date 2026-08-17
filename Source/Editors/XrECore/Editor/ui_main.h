//---------------------------------------------------------------------------
#ifndef UI_MainH
#define UI_MainH

#include "UI_MainCommand.h"
#include "IInputReceiver.h"
#include "UI_ProgressCenter.h"

// refs
class CCustomObject;
class TUI_Tools;
class TUI_Tools;
class C3DCursor;
//------------------------------------------------------------------------------

enum EEditorState{
	esNone,
    esEditScene,
    esEditLibrary,
    esEditLightAnim,
    esBuildLevel
};

struct ECORE_API SPBItem{
	shared_str	text;
    shared_str	info;
    float 		max;
    float 		progress;
	u64			started_at_ms;
	u64			last_publish_ms;
	u64			last_log_ms;
	s32			last_publish_percent;
	s32			last_log_percent;
	shared_str	last_logged_info;
public:
                SPBItem				(LPCSTR txt, LPCSTR inf, float mx);
    void		GetInfo				(xr_string& txt, float& p, float& m);
    void		Inc					(LPCSTR info=0, bool bWarn=false);
    void		Update				(float val);
	void 		Info				(LPCSTR text, bool bWarn=false);
private:
	s32			Percent				() const;
	void		Publish				(bool force, bool warning);
};

typedef xr_vector<EEditorState> EStateList;
typedef EStateList::iterator EStateIt;

struct SFramePacingStats
{
	u32 active_idle_fps;
	u32 background_poll_ms;
	float measured_fps;
	float measured_frame_ms;
	LPCSTR last_wait_reason;
	bool app_active;
	bool play_in_editor;
	bool realtime_render;
	bool redraw_pending;
	bool idle_cap_active;
	u64 frames;
	u64 waits;
	u64 waited_us;
};

struct SDeferredEditorCommand
{
	u32 command;
	CCommandVar p1;
	CCommandVar p2;
};

struct SDeferredWindowMessage
{
	HWND window;
	UINT message;
	WPARAM wparam;
	LPARAM lparam;
};

using TDeferredUIWork = void(*)();

class ECORE_API TUI: public IInputReceiver,public XrUIManager
{
    bool m_AppClosed;
    inline void	RealQuit() { m_AppClosed = true; }
	HANDLE m_FrameWaitTimer;
	HANDLE m_FrameWakeEvent;
	u64 m_FrameClockFrequency;
	u64 m_LastFrameStartedAt;
	u64 m_LastFrameIntervalTicks;
	u64 m_FramePacingFrames;
	u64 m_FramePacingWaits;
	u64 m_FramePacingWaitTicks;
	u32 m_LastFramePacingReason;
	xr_vector<SDeferredEditorCommand> m_DeferredCommands;
	xr_vector<TDeferredUIWork> m_DeferredUIWork;
	void SyncAppActivation();
	void WaitForFramePacing();
	void ProcessDeferredWork();
	// Main-thread flags remain authoritative; the event only interrupts a wait
	// when work is posted from outside the current idle iteration.
	void WakeFramePacing() { if (m_FrameWakeEvent) ::SetEvent(m_FrameWakeEvent); }
protected:
    Ivector2    m_Size;
    bool        m_Size_Maximize;
protected:
    friend class CCustomPreferences;
    friend class CEditorRenderDevice;

    TShiftState m_ShiftState;

    bool m_bAppActive;
protected:
    EStateList m_EditorState;
    bool bNeedAbort;
	u32 m_ProgressOperationDepth;
	bool m_ProgressCancelable;
	bool m_ProgressImplicitOperation;
	bool m_ProgressDeferredQuit;
	u64 m_LastProgressMessagePumpMs;
	u64 m_LastProgressPaintMs;
	xr_vector<SDeferredWindowMessage> m_DeferredWindowMessages;
	void BeginProgressOperation(bool cancelable);
	void EndProgressOperation();
	void PumpProgressMessages();
	void ReplayDeferredWindowMessages();
	void DrawProgressFrame();
	friend class SProgressOperation;
public:
	bool m_bReady;
protected:
	Fvector m_Pivot;
protected:
	bool m_SelectionRect;
	Ivector2 m_SelStart;
	Ivector2 m_SelEnd;
protected:
	enum{
    	flRedraw		= (1<<0),
        flUpdateScene	= (1<<1),
        flResize		= (1<<2),
        flNeedQuit		= (1<<3),
        flResetUI       = (1<<4),
    };
	Flags32 m_Flags;
protected:
	long m_StartTime;

    void PrepareRedraw	();
    void Redraw			();
protected:
    void D3D_CreateStateBlocks();
    void D3D_DestroyStateBlocks();
public:
    virtual void OutUICursorPos	()=0;
	virtual void OutGridSize	()=0;
	virtual void OutInfo		()=0;
public:
	// non-hidden ops
	Ivector2 m_StartCp;
	Ivector2 m_CurrentCp;

	Fvector m_CurrentRStart;
	Fvector m_CurrentRDir;


	Fvector m_StartRStart;
	Fvector m_StartRDir;

	// hidden ops
	Ivector2 m_StartCpH;
	Ivector2 m_DeltaCpH;
protected:
	bool m_MouseCaptured;
	bool m_MouseMultiClickCaptured;
	bool bMouseInUse;

	xr_string m_LastHint;
    bool  m_bHintShowing;
    POINT m_HintPoint;

    // mailslot
    HANDLE			hMailSlot;
public:
    void ShowObjectHint();
    void ShowHint(const xr_string& s);
    bool ShowHint(const AStringVec& SS);
    void HideHint();
public:
    // mouse sensetive
    float m_MouseSM, m_MouseSS, m_MouseSR;
protected:
    virtual void 	RealUpdateScene	()=0;
    void			RealRedrawScene	();
    void			RealResize		();
    void			OnFrame			();
public:
    				TUI				();
    virtual 		~TUI			();

    void			Quit			()	{	m_Flags.set(flNeedQuit,TRUE); WakeFramePacing(); }
    
    u32 			&GetRenderWidth	()	{   return EDevice->dwWidth; }
    u32&GetRenderHeight	()	{   return EDevice->dwHeight; }
    int 			GetRealWidth	()	{   return EDevice->dwRealWidth; }
    int 			GetRealHeight	()  {   return EDevice->dwRealHeight; }

    IC float 		ZFar			()	{	return EDevice->m_Camera.m_Zfar; }
    IC TShiftState	GetShiftState 	()	{	return m_ShiftState; }

    virtual bool 	OnCreate		();
    virtual void 	OnDestroy		();

    virtual char* 	GetCaption		()=0;
 
    bool 			IsModified		();

    bool  Idle			();
    void 			Resize(int x, int y, bool maximize = false, bool bForced = false) { m_Size.set(x, y); m_Size_Maximize = maximize;   m_Flags.set(flResize | flRedraw, TRUE); WakeFramePacing(); if (bForced) RealResize(); }
    void 			Resize(bool bForced = false) { m_Flags.set(flResize | flRedraw, TRUE); WakeFramePacing(); if (bForced) RealResize(); }

    // add, remove, changing objects/scene
    void 			UpdateScene			(bool bForced=false){	m_Flags.set(flUpdateScene,TRUE); WakeFramePacing();	if (bForced) RealUpdateScene();}
    // Only redraw scene. A forced redraw runs a WHOLE frame, ImGui pass and
    // all - so panel code asking for one while it is itself being drawn would
    // nest an ImGui frame inside the live one. That kills the outer frame's
    // window stack and the caller dies on its next ImGui call, which is how a
    // plain click in the outliner took the editor down. The flag alone is
    // enough there: the next Idle tick redraws anyway.
    void 			RedrawScene			(bool bForced=false){   m_Flags.set(flRedraw,TRUE); WakeFramePacing();		if (bForced && !InUIPass()) RealRedrawScene();}

    void 			SetRenderQuality	(float q)      {   EDevice->m_ScreenQuality = q;}
// mouse action
    void 			EnableSelectionRect	(bool flag );                                               
    void 			UpdateSelectionRect	(const Ivector2& from, const Ivector2& to );

    void 			MouseMultiClickCapture(bool b){m_MouseMultiClickCaptured = b;}

    bool  IsMouseCaptured		()	{	return m_MouseCaptured|m_MouseMultiClickCaptured;}
    bool  IsMouseInUse		()	{	return bMouseInUse;}

   virtual bool  KeyDown     		(WORD Key, TShiftState Shift);
    bool  KeyUp       		(WORD Key, TShiftState Shift);
    bool  KeyPress    		(WORD Key, TShiftState Shift);
	void  MousePress			(TShiftState Shift, int X, int Y);
	void  MouseRelease		(TShiftState Shift, int X, int Y);
	void  MouseMove			(TShiftState Shift, int X, int Y);
	void  MouseWheel			(TShiftState Shift, float steps);
	virtual bool IsViewportNavigating() override { return EDevice->m_Camera.IsMoving(); }

    void 			BeginEState			(EEditorState st){ m_EditorState.push_back(st); }
    void 			EndEState			(){ m_EditorState.pop_back(); }
    void 			EndEState			(EEditorState st){
    	VERIFY(std::find(m_EditorState.begin(),m_EditorState.end(),st)!=m_EditorState.end());
        for (EStateIt it=m_EditorState.end()-1; it>=m_EditorState.begin(); it--)
        	if (*it==st){
            	m_EditorState.erase(it,m_EditorState.end());
            	break;
            }
    }
    EEditorState 	GetEState			(){ return m_EditorState.back(); }
    bool 			ContainEState		(EEditorState st){ return std::find(m_EditorState.begin(),m_EditorState.end(),st)!=m_EditorState.end(); }

    virtual void 	OutCameraPos		()=0;
    virtual void 	SetStatus			(LPCSTR s, bool bOutLog=true)=0;
    virtual void 	ResetStatus			()=0;
    
	// direct input
	virtual void 	IR_OnMouseMove		(int x, int y);

    void 			OnAppActivate		();
    void 			OnAppDeactivate     ();

    bool    		NeedAbort           (){ return bNeedAbort;}
    bool 			NeedBreak			(){ return RequestProgressCancel(); }
    void 			ResetBreak			(){bNeedAbort = false;}

    virtual bool 	ApplyShortCut		(DWORD Key, TShiftState Shift)=0;
    virtual bool 	ApplyGlobalShortCut	(DWORD Key, TShiftState Shift)=0;
	virtual bool	ShouldDeferCommand	(u32 command) const;
	bool			DeferCommand		(u32 command, CCommandVar p1, CCommandVar p2);
	bool			DeferUIWork		(TDeferredUIWork work);

    void			SetGradient			(u32 color){;}

    void 			OnDeviceCreate		();
    void			OnDeviceDestroy		();

    // mailslot
#if 0
	bool 			CreateMailslot		();
	void 			CheckMailslot		();
	void 			OnReceiveMail		(LPCSTR msg);
	void 			SendMail			(LPCSTR name, LPCSTR dest, LPCSTR msg);
#endif

    void			CheckWindowPos		(HWND* form);

    virtual LPCSTR 	EditorName			()=0;
    virtual LPCSTR	EditorDesc			()=0;

// commands   
	virtual	void	RegisterCommands			()=0; 
	void			ClearCommands				();
    
	CCommandVar		CommandRenderFocus			(CCommandVar p1, CCommandVar p2);
	CCommandVar		CommandBreakLastOperation	(CCommandVar p1, CCommandVar p2);
	CCommandVar		CommandRenderResize			(CCommandVar p1, CCommandVar p2);

    virtual void	SaveSettings				(CInifile*){}
    virtual void	LoadSettings				(CInifile*){}
protected:    
// progress bar
    DEFINE_VECTOR	(SPBItem*,PBVec,PBVecIt);
    PBVec			m_ProgressItems;
	bool			m_ProgressOwnsConsole;
public:
	SPBItem*		ProgressStart		(float max_val, LPCSTR text);
	void 			ProgressEnd			(SPBItem*&);
    virtual void	ProgressDraw();
    SPBItem*		ProgressLast		(){return m_ProgressItems.empty()?0:m_ProgressItems.back();}
	void			GetProgressSnapshot	(SProgressTaskInfoVec& result) const;
	bool			ProgressOperationActive() const { return m_ProgressOperationDepth != 0; }
	bool			ProgressCancelable	() const { return ProgressOperationActive() && m_ProgressCancelable; }
	bool			RequestProgressCancel();
	void			ProgressCheckpoint	();
	void			GetFramePacingStats	(SFramePacingStats& result);

	void ShowConsole();
    void WriteConsole(TMsgDlgType mt, const char* txt);
    void CloseConsole();
public:
    ref_rt				RT;
    ref_rt				ZB;
    _vector2<u32>            RTSize;
protected:
    virtual void OnDrawUI();
	virtual void OnDrawProgressUI();
    void RealResetUI();
    HANDLE m_HConsole;
public:
   IC  void ResetUI(bool bForced=false)  { if (!bForced){ m_Flags.set(flResetUI, TRUE); WakeFramePacing(); } if (bForced) RealResetUI(); }
   virtual Ivector2 GetRenderMousePosition()const { return Ivector2().set(0, 0); }
   virtual void	OnStats(CGameFont* font);
};

class ECORE_API SProgressOperation
{
	TUI* m_UI;
public:
	explicit SProgressOperation(TUI& ui, bool cancelable);
	~SProgressOperation();
	SProgressOperation(const SProgressOperation&) = delete;
	SProgressOperation& operator=(const SProgressOperation&) = delete;
};
//---------------------------------------------------------------------------
extern ECORE_API TUI* UI;  
//---------------------------------------------------------------------------
void ECORE_API ResetActionToSelect();
#define COMMAND0(cmd)		{ExecCommand(cmd);bExec=true;}
#define COMMAND1(cmd,p0)	{ExecCommand(cmd,p0);bExec=true;}
//---------------------------------------------------------------------------
#endif
