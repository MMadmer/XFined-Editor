#pragma once

// Unreal-style asset preview: a dockable window that renders one visual into
// its own render target and shows what the file is made of. The window owns
// the model - closing it (or switching to another asset) releases it, so
// nothing stays loaded behind the user's back.
//
// The model is drawn straight into a D3D9 render-target texture from inside
// the editor's already open scene: no BeginScene/EndScene, no readback.

#if defined(USE_DX11)
#include "../../../XrECore/Editor/EDX11Utils.h"
#endif

class IRenderVisual;

class UIVisualPreview : public XrUI
{
public:
					UIVisualPreview		();
	virtual			~UIVisualPreview	();

	// opens/focuses the window and shows the model.
	//   visual_name - name for ::Render->model_Create (relative to
	//                 $game_meshes$, extension optional); null just opens the
	//                 window empty
	static void		Show				(LPCSTR visual_name = nullptr);
	// same, but the block comes from memory - assets inside the game archives
	// never hit the disk
	static void		ShowFromMemory		(LPCSTR title, const void* data, u32 size);
	static void		Close				();
	static IC bool	IsOpen				() { return !!Form; }
	static void		Update				();		// draws if open
	virtual void	Draw				();

	// The window label carries the model name, so its imgui id is pinned with
	// "###". This is the string the dock layout has to be given.
	static LPCSTR	WindowID			() { return "###VisualPreview"; }

	// D3D9 refuses to Reset() while a D3DPOOL_DEFAULT surface is alive; these
	// are wired into CEditorRenderDevice::Reset through g_pOnEditorDeviceReset*
	static void		OnDeviceResetBegin	();
	static void		OnDeviceResetEnd	();

private:
	static UIVisualPreview*	Form;

	// model
	IRenderVisual*	m_Visual;
	xr_string		m_Name;
	u32				m_Type;			// MT_*
	u32				m_Verts;
	u32				m_Tris;
	u16				m_Bones;
	Fbox			m_Box;
	Fvector			m_Center;
	float			m_Radius;
	xr_string		m_Error;

	// orbit camera, all relative to the model's AABB
	float			m_Yaw;
	float			m_Pitch;
	float			m_Distance;
	Fvector			m_Pan;

	bool			m_Wireframe;
	bool			m_ShowBones;
	bool			m_Focus;		// pull the window to front on the next draw

	// Render target, recreated only when the viewport size changes. Raw device
	// objects: the resource manager's CRT lives inside XrECore and is not exported.
#if defined(USE_DX11)
	// texture + RTV + depth + SRV in one; the SRV is what ImGui::Image gets
	SDX11Target		m_Target;
#else
	IDirect3DTexture9*	m_RTTex;		// what ImGui::Image gets
	IDirect3DSurface9*	m_RTSurf;		// mip 0 of the above, bound as RT
	IDirect3DSurface9*	m_ZBSurf;
#endif
	u32				m_RTW;
	u32				m_RTH;
	bool			m_RTFailed;		// creation refused - stop retrying every frame

	void			SetVisual			(IRenderVisual* v, LPCSTR name);
	void			ReleaseVisual		();
	void			ReleaseTarget		();
	bool			EnsureTarget		(u32 w, u32 h);
	void			ResetCamera			();
	void			RenderToTarget		(u32 w, u32 h);
	void			DrawToolBar			();
	void			DrawViewport		();
	void			DrawInfo			();
};
