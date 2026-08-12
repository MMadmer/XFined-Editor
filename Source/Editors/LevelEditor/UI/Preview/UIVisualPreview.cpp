#include "stdafx.h"

#include "UIVisualPreview.h"
#include "../../../../XrRender/Private/FVisual.h"
#include "../../../../XrRender/Private/FHierrarhyVisual.h"
#include "../../../../XrRender/Public/KinematicsAnimated.h"
// library objects (.object) draw through CEditableObject::RenderSingle
#include "../../../XrECore/Editor/EditObject.h"

extern BOOL ENGINE_API g_bRendering;

UIVisualPreview* UIVisualPreview::Form = nullptr;

namespace
{
	// Three-quarter shot from above, same framing the asset thumbnails use:
	// a flat front view flattens most props into unreadable silhouettes.
	static const Fvector	s_StartOffset		= {  1.0f,  0.8f, -1.0f };
	static const float		s_FOVDegrees		= 45.f;
	static const u32		s_BackgroundColor	= 0xff2b2b2b;
	static const float		s_OrbitSpeed		= 0.008f;	// rad per pixel
	static const float		s_PitchLimit		= 1.5533f;	// ~89 deg
	static const float		s_JointSize			= 0.02f;
	static const u32		s_JointColor		= 0xffffff00;
	static const u32		s_LinkColor			= 0xffa0a000;

	//--------------------------------------------------------------------------
	// We are a guest inside the editor's frame - the imgui pass runs between
	// EDevice->Begin() and End() - so everything we poke gets captured on entry
	// and pushed back on exit, exception or not.
	//--------------------------------------------------------------------------
	struct SPreviewStateGuard
	{
#if defined(USE_DX11)
		// D3D11 has no state blocks: SDX11TargetGuard captures the render target,
		// depth view and viewport, and the shader state is re-applied per draw
		// anyway once RCache is invalidated below.
		SDX11TargetGuard		m_Views;
		// lights/material live in the recorded FF block now - restore it so the
		// preview's light rig does not leak into the scene pass
		SEditorFixedFunc		m_FF;
#else
		IDirect3DStateBlock9*	m_Block;
		IDirect3DSurface9*		m_OldRT;
		IDirect3DSurface9*		m_OldZB;
		D3DVIEWPORT9			m_OldViewport;
#endif
		Fmatrix					m_OldWorld;
		Fmatrix					m_OldView;
		Fmatrix					m_OldProject;
		Fmatrix					m_OldDeviceView;
		Fmatrix					m_OldDeviceProject;
		Fmatrix					m_OldDeviceFull;
		Fvector					m_OldCameraPos;
		Fvector					m_OldCameraDir;
		CFrustum				m_OldFrustum;

		SPreviewStateGuard()
		{
#if defined(USE_DX11)
			m_FF				= EDevice->ff;
#else
			m_Block				= 0;
			m_OldRT				= 0;
			m_OldZB				= 0;
			// D3DSBT_ALL covers render states, samplers, lights and material -
			// the shader passes we are about to run overwrite all of those
			HW.pDevice->CreateStateBlock	(D3DSBT_ALL,&m_Block);
			HW.pDevice->GetRenderTarget		(0,&m_OldRT);
			HW.pDevice->GetDepthStencilSurface(&m_OldZB);
			HW.pDevice->GetViewport			(&m_OldViewport);
#endif
			// transforms travel through RCache, which mirrors them into the
			// device, so save/restore has to go through RCache as well
			m_OldWorld			= RCache.get_xform_world	();
			m_OldView			= RCache.get_xform_view		();
			m_OldProject		= RCache.get_xform_project	();
			m_OldDeviceView		= EDevice->mView;
			m_OldDeviceProject	= EDevice->mProject;
			m_OldDeviceFull		= EDevice->mFullTransform;
			m_OldCameraPos		= EDevice->vCameraPosition;
			m_OldCameraDir		= EDevice->vCameraDirection;
			// CRender shadows IRender_interface::ViewBase, and the culling in
			// CModelPool goes through CRender::occ_visible - so the frustum
			// that matters is the one on RImplementation, not on ::Render
			m_OldFrustum		= RImplementation.ViewBase;
		}

		~SPreviewStateGuard()
		{
#if defined(USE_DX11)
			EDevice->ff			= m_FF;
#else
			if (m_Block)		m_Block->Apply();
			// render targets are not part of a D3D9 state block - restore by
			// hand, and only then the viewport, since SetRenderTarget resets it
			// (slot 0 refuses a NULL surface, so only push a real one back)
			if (m_OldRT)	HW.pDevice->SetRenderTarget(0,m_OldRT);
			HW.pDevice->SetDepthStencilSurface	(m_OldZB);
			HW.pDevice->SetViewport				(&m_OldViewport);
#endif
			RCache.set_xform_world				(m_OldWorld);
			RCache.set_xform_view				(m_OldView);
			RCache.set_xform_project			(m_OldProject);
			EDevice->mView						= m_OldDeviceView;
			EDevice->mProject					= m_OldDeviceProject;
			EDevice->mFullTransform				= m_OldDeviceFull;
			EDevice->vCameraPosition			= m_OldCameraPos;
			EDevice->vCameraDirection			= m_OldCameraDir;
			RImplementation.ViewBase			= m_OldFrustum;
			// the state block put the device somewhere RCache does not know
			// about - OnFrameEnd is the public way to drop the cache, so the
			// next draw re-applies everything
			RCache.OnFrameEnd					();
#if !defined(USE_DX11)
			_RELEASE							(m_Block);
			_RELEASE							(m_OldRT);
			_RELEASE							(m_OldZB);
#endif
			// on D3D11 m_Views puts the render target, depth view and viewport
			// back when it goes out of scope, right after this body
		}
	};

	//--------------------------------------------------------------------------
	// The editor draws visuals through the fixed function T&L pipe, so the model
	// needs real lights. Same rig the actor editor preview uses.
	//--------------------------------------------------------------------------
	void SetupPreviewLights()
	{
		Flight L;
		ZeroMemory		(&L,sizeof(L));
		L.type			= D3DLIGHT_DIRECTIONAL;

		L.diffuse.set	(1.f,1.f,1.f,1.f);
		L.direction.set	(1,-1,1);		L.direction.normalize();
		EDevice->SetLight(0,L);			EDevice->LightEnable(0,TRUE);

		L.diffuse.set	(0.35f,0.35f,0.4f,1.f);
		L.direction.set	(-1,-1,-1);		L.direction.normalize();
		EDevice->SetLight(1,L);			EDevice->LightEnable(1,TRUE);

		L.diffuse.set	(0.3f,0.3f,0.3f,1.f);
		L.direction.set	(-1,1,-1);		L.direction.normalize();
		EDevice->SetLight(2,L);			EDevice->LightEnable(2,TRUE);

		L.diffuse.set	(0.25f,0.25f,0.25f,1.f);
		L.direction.set	(1,1,-1);		L.direction.normalize();
		EDevice->SetLight(3,L);			EDevice->LightEnable(3,TRUE);

		for (u32 i=4; i<8; i++)
			EDevice->LightEnable(i,FALSE);
	}

	//--------------------------------------------------------------------------
	void SetupPreviewStates(bool wireframe)
	{
		// states the shader passes do not own themselves: the editor may sit in
		// wireframe/flat/fog mode and that must not leak into the preview
		// through the device wrapper: on D3D9 it writes the state right away, on
		// D3D11 it records it for the fixed-function emulation
		EDevice->SetRS(D3DRS_COLORWRITEENABLE,	D3DCOLORWRITEENABLE_ALPHA|D3DCOLORWRITEENABLE_BLUE|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_RED);
		EDevice->SetRS(D3DRS_FILLMODE,			wireframe?D3DFILL_WIREFRAME:D3DFILL_SOLID);
		EDevice->SetRS(D3DRS_SHADEMODE,			D3DSHADE_GOURAUD);
		EDevice->SetRS(D3DRS_FOGENABLE,			FALSE);
		EDevice->SetRS(D3DRS_AMBIENT,			0x30303030);
		EDevice->SetRS(D3DRS_TEXTUREFACTOR,		0xffffffff);
		for (u32 k=0; k<HW.Caps.raster.dwStages; k++)
		{
			EDevice->SetSS(k,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
			EDevice->SetSS(k,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
			EDevice->SetSS(k,D3DSAMP_MIPFILTER,D3DTEXF_LINEAR);
		}
		EDevice->ResetMaterial();
	}

	//--------------------------------------------------------------------------
	// CModelPool::Instance_Create FATALs on visual types the editor does not
	// build (MT_LOD, MT_TREE_*), so anything unknown has to be rejected before
	// the pool ever sees it.
	//--------------------------------------------------------------------------
	bool IsSupportedVisualType(u8 type)
	{
		switch (type)
		{
		case MT_NORMAL:
		case MT_HIERRARHY:
		case MT_PROGRESSIVE:
		case MT_SKELETON_ANIM:
		case MT_SKELETON_RIGID:
		case MT_SKELETON_GEOMDEF_PM:
		case MT_SKELETON_GEOMDEF_ST:
			return true;
		default:
			return false;
		}
	}

	LPCSTR VisualTypeName(u32 type)
	{
		switch (type)
		{
		case MT_NORMAL:					return "static mesh";
		case MT_HIERRARHY:				return "hierarchy";
		case MT_PROGRESSIVE:			return "progressive mesh";
		case MT_SKELETON_ANIM:			return "animated skeleton";
		case MT_SKELETON_RIGID:			return "rigid skeleton";
		case MT_SKELETON_GEOMDEF_PM:	return "skinned mesh (PM)";
		case MT_SKELETON_GEOMDEF_ST:	return "skinned mesh (ST)";
		case MT_LOD:					return "LOD";
		case MT_TREE_ST:				return "tree (ST)";
		case MT_TREE_PM:				return "tree (PM)";
		case MT_PARTICLE_EFFECT:		return "particle effect";
		case MT_PARTICLE_GROUP:			return "particle group";
		default:						return "unknown";
		}
	}

	// CModelPool::Instance_Load feeds the block straight into r_chunk_safe,
	// which R_ASSERTs on a missing or mis-sized header. Peek it ourselves first.
	bool PeekVisualType(IReader* data, u8& type)
	{
		if (!data)								return false;
		ogf_header H;
		if (sizeof(H)!=data->find_chunk(OGF_HEADER))	{ data->rewind(); return false; }
		data->r		(&H,sizeof(H));
		data->rewind();
		type = H.type;
		return true;
	}

	// Same lookup CModelPool::Instance_Load does, so we can peek the header
	// without letting the pool load a file it will choke on.
	bool ResolveVisualFile(LPCSTR N, string_path& fn)
	{
		string_path name;
		if (0==strext(N))	strconcat	(sizeof(name),name,N,".ogf");
		else				xr_strcpy	(name,sizeof(name),N);

		if (FS.exist(N))						{ xr_strcpy(fn,sizeof(fn),N); return true; }
		if (FS.exist(fn,"$level$",name))		return true;
		if (FS.exist(fn,"$game_meshes$",name))	return true;
		return false;
	}

	bool IsDeviceUsable()
	{
		return (0!=EDevice)&&(FALSE!=EDevice->b_is_Ready)&&(0!=HW.pDevice);
	}

	//--------------------------------------------------------------------------
	// Geometry stats. dxRender_Visual is the only exported node type, so the
	// walk switches on getType() and static_casts - no RTTI across the DLL
	// boundary and no guessing about the concrete class.
	//--------------------------------------------------------------------------
	void CountGeometry(dxRender_Visual* v, u32& verts, u32& tris)
	{
		if (!v)	return;
		switch (v->getType())
		{
		case MT_HIERRARHY:
		case MT_SKELETON_ANIM:
		case MT_SKELETON_RIGID:
			{
				FHierrarhyVisual* h = (FHierrarhyVisual*)v;
				for (u32 i=0; i<h->children.size(); i++)
					CountGeometry(h->children[i],verts,tris);
			}
			break;
		case MT_NORMAL:
		case MT_PROGRESSIVE:
		case MT_SKELETON_GEOMDEF_ST:
		case MT_SKELETON_GEOMDEF_PM:
			{
				Fvisual* f = (Fvisual*)v;
				verts	+= f->vCount;
				tris	+= f->dwPrimitives;
			}
			break;
		default:
			break;
		}
	}

	// Animated skeletons with no active blend collapse into a heap: the bone
	// transforms never leave identity. Any cycle at all fixes the pose, so take
	// the first motion the model ships (idle when it has one).
	void PoseSkeleton(IRenderVisual* visual)
	{
		if (IKinematicsAnimated* KA = visual->dcast_PKinematicsAnimated())
		{
			MotionID motion = KA->LL_MotionID("idle");
			for (u16 slot = 0; !motion.valid() && slot < KA->LL_MotionsSlotCount(); ++slot)
			{
				// ref-counted handle: the accessors are non-const, and the copy
				// keeps the motions alive for the lookup
				shared_motions ms = KA->LL_MotionsSlot(slot);
				if (accel_map* cycles = ms.cycle())
					if (!cycles->empty())
						motion = KA->LL_MotionID(*cycles->begin()->first);
			}
			if (motion.valid())
				KA->LL_PlayCycle(0, motion, TRUE, 0, 0);
		}

		// A skeleton's vis.box straight out of the pool is the union over every
		// animation, so framing by it leaves the model tiny and off-centre.
		// CalculateBones fits the box to the pose we are about to draw.
		if (IKinematics* K = visual->dcast_PKinematics())
			K->CalculateBones(TRUE);
	}
}
//------------------------------------------------------------------------------

UIVisualPreview::UIVisualPreview()
{
	m_Visual		= nullptr;
	m_Editable		= nullptr;
	m_Type			= u32(-1);
	m_Verts			= 0;
	m_Tris			= 0;
	m_Bones			= 0;
	m_Box.invalidate();
	m_Center.set	(0.f,0.f,0.f);
	m_Radius		= 1.f;
	m_Yaw			= 0.f;
	m_Pitch			= 0.f;
	m_Distance		= 1.f;
	m_Pan.set		(0.f,0.f,0.f);
	m_Wireframe		= false;
	m_ShowBones		= false;
	m_Focus			= true;
#if !defined(USE_DX11)
	m_RTTex			= nullptr;
	m_RTSurf		= nullptr;
	m_ZBSurf		= nullptr;
#endif
	m_RTW			= 0;
	m_RTH			= 0;
	m_RTFailed		= false;
	ResetCamera		();
}

UIVisualPreview::~UIVisualPreview()
{
	ReleaseVisual	();
	ReleaseTarget	();
}

//------------------------------------------------------------------------------
// lifetime
//------------------------------------------------------------------------------
void UIVisualPreview::Show(LPCSTR visual_name)
{
	if (!Form) Form = xr_new<UIVisualPreview>();
	Form->bOpen		= true;
	Form->m_Focus	= true;

	if (!visual_name||!visual_name[0])	return;
	// already showing this one - just bring the window forward
	if ((Form->m_Visual||Form->m_Editable)&&(0==xr_strcmp(Form->m_Name.c_str(),visual_name)))	return;

	Form->ReleaseVisual	();
	Form->m_Name		= visual_name;

	if (!IsDeviceUsable())	{ Form->m_Error = "render device is not ready"; return; }

	string_path fn;
	if (!ResolveVisualFile(visual_name,fn))
	{
		// no engine visual under that name - it may still be a library object,
		// which is what the Objects category of the content browser hands us
		string_path obj_fn;
		FS.update_path	(obj_fn,_objects_,EFS.ChangeFileExt(visual_name,".object").c_str());
		if (FS.exist(obj_fn))	{ ShowObject(visual_name); return; }

		Form->m_Error = "file not found";
		return;
	}

	IReader* test = FS.r_open(fn);
	if (!test)								{ Form->m_Error = "cannot open file"; return; }
	u8	 type		= u8(-1);
	bool type_ok	= PeekVisualType(test,type);
	FS.r_close		(test);
	if (!type_ok)							{ Form->m_Error = "not an OGF model"; return; }
	if (!IsSupportedVisualType(type))
	{
		string64 msg; xr_sprintf(msg,sizeof(msg),"unsupported visual type (%s)",VisualTypeName(type));
		Form->m_Error = msg;
		return;
	}

	// CModelPool::Delete refuses to discard while the engine believes a frame is
	// being rendered; this load is entirely our own, so clear the flag for it
	const BOOL saved_rendering = g_bRendering;
	g_bRendering	= FALSE;
	IRenderVisual* v = ::Render->model_Create(visual_name);
	g_bRendering	= saved_rendering;

	if (!v)	{ Form->m_Error = "model_Create failed"; return; }
	Form->SetVisual	(v,visual_name);
}

void UIVisualPreview::ShowFromMemory(LPCSTR title, const void* data, u32 size)
{
	if (!Form) Form = xr_new<UIVisualPreview>();
	Form->bOpen		= true;
	Form->m_Focus	= true;

	Form->ReleaseVisual	();
	Form->m_Name		= (title&&title[0])?title:"<memory>";

	if (!data||(0==size))	{ Form->m_Error = "empty block"; return; }
	if (!IsDeviceUsable())	{ Form->m_Error = "render device is not ready"; return; }

	IReader reader	((void*)data,int(size));
	u8	 type		= u8(-1);
	if (!PeekVisualType(&reader,type))	{ Form->m_Error = "not an OGF model"; return; }
	if (!IsSupportedVisualType(type))
	{
		string64 msg; xr_sprintf(msg,sizeof(msg),"unsupported visual type (%s)",VisualTypeName(type));
		Form->m_Error = msg;
		return;
	}

	// The pool keys models by name and returns the cached copy on a hit, which
	// would silently ignore our block. Generate a private key instead - and keep
	// it dot-free, CModelPool::Create truncates the key at the last '.'.
	static u32	s_uid = 0;
	string_path	pool_key;
	xr_sprintf	(pool_key,sizeof(pool_key),"$preview$%u",++s_uid);

	const BOOL saved_rendering = g_bRendering;
	g_bRendering	= FALSE;
	IRenderVisual* v = ::Render->model_Create(pool_key,&reader);
	g_bRendering	= saved_rendering;

	if (!v)	{ Form->m_Error = "model_Create failed"; return; }
	Form->SetVisual	(v,Form->m_Name.c_str());
}

void UIVisualPreview::ShowObject(LPCSTR object_name)
{
	if (!Form) Form = xr_new<UIVisualPreview>();
	Form->bOpen		= true;
	Form->m_Focus	= true;

	if (!object_name||!object_name[0])	return;
	if ((Form->m_Visual||Form->m_Editable)&&(0==xr_strcmp(Form->m_Name.c_str(),object_name)))	return;

	Form->ReleaseVisual	();
	Form->m_Name		= object_name;

	if (!IsDeviceUsable())	{ Form->m_Error = "render device is not ready"; return; }

	string_path fn;
	FS.update_path	(fn,_objects_,EFS.ChangeFileExt(object_name,".object").c_str());
	if (!FS.exist(fn))	{ Form->m_Error = "file not found"; return; }

	// a private copy, exactly like the thumbnail renderer: the shared library
	// instance is not ours to hold on to across frames
	CEditableObject* obj = xr_new<CEditableObject>(object_name);
	if (!obj->Load(fn))	{ xr_delete(obj); Form->m_Error = "cannot load object"; return; }

	Form->SetEditable	(obj,object_name);
}

void UIVisualPreview::Close()
{
	xr_delete(Form);
}

void UIVisualPreview::Update()
{
	if (!Form) return;
	Form->Draw();
	if (Form->IsClosed()) Close();
}

//------------------------------------------------------------------------------
// model
//------------------------------------------------------------------------------
void UIVisualPreview::SetVisual(IRenderVisual* v, LPCSTR name)
{
	m_Visual	= v;
	m_Name		= name?name:"";
	m_Error		= "";
	m_Type		= v->getType();
	m_Verts		= 0;
	m_Tris		= 0;
	m_Bones		= 0;

	PoseSkeleton	(v);
	CountGeometry	(static_cast<dxRender_Visual*>(v),m_Verts,m_Tris);
	if (IKinematics* K = v->dcast_PKinematics())	m_Bones = K->LL_BoneCount();

	m_Box		= v->getVisData().box;
	ResetCamera	();
}

void UIVisualPreview::SetEditable(CEditableObject* o, LPCSTR name)
{
	m_Editable	= o;
	m_Name		= name?name:"";
	m_Error		= "";
	// a library object is not an engine visual, so there is no MT_* for it;
	// DrawInfo reports the kind from m_Editable instead
	m_Type		= u32(-1);
	m_Verts		= u32(o->GetVertexCount());
	m_Tris		= u32(o->GetFaceCount());
	m_Bones		= u16(o->BoneCount());

	m_Box		= o->GetBox();
	ResetCamera	();
}

void UIVisualPreview::ReleaseVisual()
{
	if (m_Visual)
	{
		// the pool refuses to discard mid-frame, and this can run from anywhere
		const BOOL saved_rendering = g_bRendering;
		g_bRendering	= FALSE;
		::Render->model_Delete(m_Visual,TRUE);
		g_bRendering	= saved_rendering;
	}
	xr_delete	(m_Editable);
	m_Visual	= nullptr;
	m_Type		= u32(-1);
	m_Verts		= 0;
	m_Tris		= 0;
	m_Bones		= 0;
	m_Box.invalidate();
	m_Error		= "";
}

//------------------------------------------------------------------------------
// render target
//------------------------------------------------------------------------------
void UIVisualPreview::ReleaseTarget()
{
#if defined(USE_DX11)
	m_Target.release();
#else
	_RELEASE	(m_ZBSurf);
	_RELEASE	(m_RTSurf);
	_RELEASE	(m_RTTex);
#endif
	m_RTW		= 0;
	m_RTH		= 0;
	m_RTFailed	= false;
}

bool UIVisualPreview::EnsureTarget(u32 w, u32 h)
{
#if defined(USE_DX11)
	if (m_Target.valid()&&(m_RTW==w)&&(m_RTH==h))				return true;
#else
	if (m_RTTex&&m_RTSurf&&m_ZBSurf&&(m_RTW==w)&&(m_RTH==h))	return true;
#endif
	// a refused size must not be retried every frame
	if (m_RTFailed&&(m_RTW==w)&&(m_RTH==h))						return false;

	ReleaseTarget	();
	m_RTW	= w;
	m_RTH	= h;

#if defined(USE_DX11)
	// with_srv: ImGui samples the result straight from this texture
	bool ok = m_Target.create(w,h,true);
	if (!ok)	m_Target.release();
#else
	// D3DPOOL_DEFAULT is the only pool a render target may live in - hence the
	// device-reset hook, see OnDeviceResetBegin()
	bool ok = SUCCEEDED(HW.pDevice->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&m_RTTex,0))&&(0!=m_RTTex);
	if (ok)	ok = SUCCEEDED(m_RTTex->GetSurfaceLevel(0,&m_RTSurf))&&(0!=m_RTSurf);
	if (ok)	ok = SUCCEEDED(HW.pDevice->CreateDepthStencilSurface(w,h,HW.Caps.bStencil?D3DFMT_D24S8:D3DFMT_D24X8,D3DMULTISAMPLE_NONE,0,FALSE,&m_ZBSurf,0))&&(0!=m_ZBSurf);
	if (!ok)
	{
		_RELEASE	(m_ZBSurf);
		_RELEASE	(m_RTSurf);
		_RELEASE	(m_RTTex);
	}
#endif

	if (!ok)
	{
		// keep the size cached so the latch above can compare against it
		m_RTW		= w;
		m_RTH		= h;
		m_RTFailed	= true;
	}
	return ok;
}

void UIVisualPreview::OnDeviceResetBegin()
{
	// the surfaces have to be gone before HW.Reset, the next Draw() rebuilds
	// them at whatever size the window ends up with
	if (Form) Form->ReleaseTarget();
}

void UIVisualPreview::OnDeviceResetEnd()
{
}

//------------------------------------------------------------------------------
// camera
//------------------------------------------------------------------------------
void UIVisualPreview::ResetCamera()
{
	m_Pan.set	(0.f,0.f,0.f);

	if (m_Box.min.x>m_Box.max.x||m_Box.min.y>m_Box.max.y||m_Box.min.z>m_Box.max.z)
	{
		m_Center.set(0.f,0.f,0.f);
		m_Radius	= 1.f;
	}
	else
	{
		m_Box.getcenter	(m_Center);
		Fvector size;	m_Box.getsize(size);
		m_Radius		= 0.5f*size.magnitude();
		if (m_Radius<EPS_L)	m_Radius = EPS_L;
	}

	Fvector dir	= s_StartOffset;	dir.normalize();
	m_Pitch		= asinf(dir.y);
	m_Yaw		= atan2f(dir.x,dir.z);
	// frame the bounding sphere with a small margin
	m_Distance	= m_Radius/_sin(deg2rad(s_FOVDegrees)*0.5f)*1.12f;
}

//------------------------------------------------------------------------------
// drawing
//------------------------------------------------------------------------------
void UIVisualPreview::RenderToTarget(u32 w, u32 h)
{
	if ((!m_Visual&&!m_Editable)||!IsDeviceUsable())	return;
#if defined(USE_DX11)
	if (!m_Target.valid())				return;

	SPreviewStateGuard	guard;

	// Invalidate, NOT OnFrameEnd: under D3D11 OnFrameEnd calls ClearState(),
	// which also drops the viewport, and nothing would set one again before the
	// draw - everything gets clipped and the preview stays empty. Invalidate
	// only forgets the cache, so the bind below is what the device ends up with.
	RCache.Invalidate();

	// bind() also sets a full-size viewport, so w/h come from the target itself
	m_Target.bind	();
	m_Target.clear	(s_BackgroundColor);
	// and now tell the cache what the device is actually bound to
	RCache.set_RT	(m_Target.rtv);
	RCache.set_ZB	(m_Target.dsv);
#else
	if (!m_RTSurf||!m_ZBSurf)			return;

	SPreviewStateGuard	guard;

	if (FAILED(HW.pDevice->SetRenderTarget(0,m_RTSurf)))		return;
	if (FAILED(HW.pDevice->SetDepthStencilSurface(m_ZBSurf)))	return;

	D3DVIEWPORT9 vp;
	vp.X = 0; vp.Y = 0; vp.Width = w; vp.Height = h; vp.MinZ = 0.f; vp.MaxZ = 1.f;
	HW.pDevice->SetViewport	(&vp);
	HW.pDevice->Clear		(0,0,D3DCLEAR_ZBUFFER|D3DCLEAR_TARGET|(HW.Caps.bStencil?D3DCLEAR_STENCIL:0),s_BackgroundColor,1.f,0);
#endif

	// imgui (or the scene pass) may have written device state behind RCache's
	// back - clear the cache so our draw sets everything up itself
	// (the D3D11 path already did this above, before binding)
#if !defined(USE_DX11)
	RCache.OnFrameEnd		();
#endif
	SetupPreviewStates		(m_Wireframe);
	SetupPreviewLights		();

	Fvector center	= m_Center;	center.add(m_Pan);
	Fvector dir;
	dir.x	= _cos(m_Pitch)*_sin(m_Yaw);
	dir.y	= _sin(m_Pitch);
	dir.z	= _cos(m_Pitch)*_cos(m_Yaw);
	Fvector eye;	eye.mad(center,dir,m_Distance);

	const float fov		= deg2rad(s_FOVDegrees);
	const float znear	= _max(0.01f,m_Distance-m_Radius*2.f);
	const float zfar	= m_Distance+m_Radius*4.f+1.f;

	Fmatrix mV, mP;
	mV.build_camera		(eye,center,Fvector().set(0.f,1.f,0.f));
	// X-Ray takes the aspect as height/width
	mP.build_projection	(fov,float(h)/float(w),znear,zfar);

	RCache.set_xform_world	(Fidentity);
	RCache.set_xform_view	(mV);
	RCache.set_xform_project(mP);
	EDevice->mView			= mV;
	EDevice->mProject		= mP;
	EDevice->mFullTransform.mul(mP,mV);
	EDevice->vCameraPosition= eye;
	EDevice->vCameraDirection.sub(center,eye).normalize_safe();
	// CModelPool::Render culls through CRender::occ_visible - without this the
	// model is thrown away before it is ever drawn
	RImplementation.ViewBase.CreateFromMatrix(EDevice->mFullTransform,FRUSTUM_P_ALL);

	if (m_Visual)	RImplementation.model_RenderSingle(m_Visual,Fidentity,1.f);
	else			m_Editable->RenderSingle(Fidentity);

	if (m_ShowBones&&m_Visual)
	{
		if (IKinematics* K = m_Visual->dcast_PKinematics())
		{
			EDevice->SetShader		(EDevice->m_WireShader);
			RCache.set_xform_world	(Fidentity);
			// scale the markers to the model, otherwise they either vanish or
			// swallow the mesh depending on how big the asset is
			const float js = m_Radius*s_JointSize*2.f;
			const u16 cnt = K->LL_BoneCount();
			for (u16 b=0; b<cnt; ++b)
			{
				const Fmatrix& M = K->LL_GetTransform(b);
				DU_impl.DrawJoint(M.c,js,s_JointColor);
				const u16 parent = K->LL_GetData(b).GetParentID();
				if (u16(-1)!=parent)
					DU_impl.DrawLine(K->LL_GetTransform(parent).c,M.c,s_LinkColor);
			}
		}
	}
}

void UIVisualPreview::DrawToolBar()
{
	if (ImGui::Button("Reset View"))	ResetCamera();
	ImGui::SameLine();
	ImGui::Checkbox("Wireframe",&m_Wireframe);
	if (m_Bones)
	{
		ImGui::SameLine();
		ImGui::Checkbox("Bones",&m_ShowBones);
	}
	ImGui::SameLine();
	if (ImGui::Button("Unload"))
	{
		ReleaseVisual();
		m_Name = "";
	}
}

void UIVisualPreview::DrawViewport()
{
	const float info_h	= ImGui::GetTextLineHeightWithSpacing()*3.f+ImGui::GetStyle().ItemSpacing.y*2.f;
	ImVec2 avail		= ImGui::GetContentRegionAvail();
	ImVec2 canvas_size	(avail.x,avail.y-info_h);
	if (canvas_size.x<64.f)	canvas_size.x = 64.f;
	if (canvas_size.y<64.f)	canvas_size.y = 64.f;

	const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();

	// the hit area goes down first so navigation is applied before the frame is
	// rendered - otherwise every drag lags one frame behind the cursor
	ImGui::InvisibleButton("##preview_canvas",canvas_size,
		ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight|ImGuiButtonFlags_MouseButtonMiddle);

	const bool hovered	= ImGui::IsItemHovered();
	const bool active	= ImGui::IsItemActive();

	if (!m_Visual&&!m_Editable)
	{
		// nothing loaded - do not sit on a render target either
		ReleaseTarget	();
		ImGui::GetWindowDrawList()->AddRectFilled(canvas_pos,
			ImVec2(canvas_pos.x+canvas_size.x,canvas_pos.y+canvas_size.y),s_BackgroundColor);
		return;
	}

	if (active)
	{
		const ImVec2 d = ImGui::GetIO().MouseDelta;
		if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)||ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			// 1:1 screen panning: how much world one pixel covers at the pivot
			const float per_px = 2.f*m_Distance*tanf(deg2rad(s_FOVDegrees)*0.5f)/canvas_size.y;
			Fvector fwd;	fwd.set(-_cos(m_Pitch)*_sin(m_Yaw),-_sin(m_Pitch),-_cos(m_Pitch)*_cos(m_Yaw));
			Fvector right;	right.crossproduct(Fvector().set(0.f,1.f,0.f),fwd);	right.normalize_safe();
			Fvector up;		up.crossproduct(fwd,right);							up.normalize_safe();
			m_Pan.mad	(right,-d.x*per_px);
			m_Pan.mad	(up,    d.y*per_px);
		}
		else if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			// drag right turns the model to the right: the camera has to orbit
			// the other way, which is +d.x for this yaw convention
			m_Yaw	+= d.x*s_OrbitSpeed;
			m_Pitch	+= d.y*s_OrbitSpeed;
			if (m_Pitch> s_PitchLimit)	m_Pitch =  s_PitchLimit;
			if (m_Pitch<-s_PitchLimit)	m_Pitch = -s_PitchLimit;
		}
	}
	if (hovered)
	{
		const float wheel = ImGui::GetIO().MouseWheel;
		if (wheel!=0.f)
		{
			m_Distance *= expf(-wheel*0.15f);
			const float lo = m_Radius*0.05f, hi = m_Radius*100.f;
			if (m_Distance<lo)	m_Distance = lo;
			if (m_Distance>hi)	m_Distance = hi;
		}
	}

	const u32 w = u32(canvas_size.x), h = u32(canvas_size.y);
	if (EnsureTarget(w,h))
	{
		RenderToTarget			(w,h);
		ImGui::SetCursorScreenPos(canvas_pos);
#if defined(USE_DX11)
		ImGui::Image			(m_Target.srv,canvas_size);
#else
		ImGui::Image			(m_RTTex,canvas_size);
#endif
	}
	else
	{
		ImGui::SetCursorScreenPos(canvas_pos);
		ImGui::TextDisabled		("cannot create a %ux%u render target",w,h);
	}
}

void UIVisualPreview::DrawInfo()
{
	ImGui::Separator();
	if (!m_Error.empty())
	{
		ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f),"%s: %s",m_Name.c_str(),m_Error.c_str());
		return;
	}
	if (!m_Visual&&!m_Editable)
	{
		ImGui::TextDisabled("no model - double click an asset in the content browser");
		return;
	}

	// library objects carry no MT_* type - name the format instead
	LPCSTR	kind	= m_Editable ? "library object" : VisualTypeName(m_Type);
	Fvector	size;	m_Box.getsize(size);
	Fvector	centre;	m_Box.getcenter(centre);

	if (ImGui::BeginTable("##modelinfo", 2, ImGuiTableFlags_SizingFixedFit))
	{
		#define ROW(k, ...)	do {											\
			ImGui::TableNextRow(); ImGui::TableNextColumn();				\
			ImGui::TextDisabled(k); ImGui::TableNextColumn();			\
			ImGui::Text(__VA_ARGS__); } while(0)

		ROW("Asset",		"%s", m_Name.c_str());
		ROW("Kind",			"%s", kind);
		ROW("Geometry",		"%u verts, %u tris", m_Verts, m_Tris);
		if (m_Bones)	ROW("Bones", "%u", u32(m_Bones));

		ROW("Bounds",		"%.2f x %.2f x %.2f", size.x, size.y, size.z);
		ROW("Centre",		"%.2f, %.2f, %.2f", centre.x, centre.y, centre.z);
		ROW("Radius",		"%.2f", m_Radius);

		if (m_Editable)
		{
			ROW("Meshes",	"%d", m_Editable->MeshCount());
			ROW("Materials","%d", m_Editable->SurfaceCount());
			// same null-when-empty shared_str trap as the material table below
			LPCSTR lods = m_Editable->GetLODs();
			if (lods && lods[0])	ROW("LOD", "%s", lods);
			if (m_Editable->IsSkeleton())
				ROW("Motions", "%d", m_Editable->SMotionCount());
		}
		#undef ROW
		ImGui::EndTable();
	}

	// Surfaces are what a static mesh IS to whoever has to ship it: which
	// shader, which texture, which game material. Unreal puts the same list in
	// its material slots panel.
	if (m_Editable && m_Editable->SurfaceCount())
	{
		if (ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::BeginTable("##surfaces", 4,
				ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Slot");
				ImGui::TableSetupColumn("Texture");
				ImGui::TableSetupColumn("Shader");
				ImGui::TableSetupColumn("Game material");
				ImGui::TableHeadersRow();

				// A surface field that was never set is an EMPTY shared_str, and
				// dereferencing one yields a NULL pointer, which TextUnformatted
				// walks straight into. Several of these are legitimately empty -
				// a game material is optional, for one.
				#define CELL(x)	do {										\
					ImGui::TableNextColumn();								\
					LPCSTR _s = (x);										\
					if (_s && _s[0])	ImGui::TextUnformatted(_s);			\
					else				ImGui::TextDisabled("-");			\
					} while(0)

				SurfaceVec& sv = m_Editable->Surfaces();
				for (u32 i = 0; i < sv.size(); ++i)
				{
					CSurface* s = sv[i];
					if (!s) continue;
					ImGui::TableNextRow();
					CELL(s->_Name());
					CELL(s->_Texture());
					CELL(s->_ShaderName());
					CELL(s->_GameMtlName());
				}
				#undef CELL
				ImGui::EndTable();
			}
		}
	}
}

void UIVisualPreview::Draw()
{
	// the label carries the model name, the "###" tail pins the window id -
	// built as a string so a long asset name can never truncate that tail away
	xr_string title("Model Preview");
	if (!m_Name.empty())	{ title += ": "; title += m_Name; }
	title += WindowID();

	if (m_Focus)
	{
		ImGui::SetNextWindowFocus();
		m_Focus = false;
	}
	ImGui::PushStyleVar	(ImGuiStyleVar_WindowMinSize,ImVec2(320,280));
	ImGui::SetNextWindowSize(ImVec2(520,520),ImGuiCond_FirstUseEver);
	// the wheel belongs to the viewport zoom, not to the window's scrolling
	if (!ImGui::Begin(title.c_str(),&bOpen,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::PopStyleVar(1);
		ImGui::End();
		return;
	}

	DrawToolBar	();
	DrawViewport();
	DrawInfo	();

	ImGui::PopStyleVar(1);
	ImGui::End();
}

//------------------------------------------------------------------------------
// Wiring. Both targets are plain pointers with no dynamic initialiser of their
// own, so claiming them from a static object here cannot be undone later.
//------------------------------------------------------------------------------
namespace
{
	struct SVisualPreviewHooks
	{
		SVisualPreviewHooks()
		{
			// content browser: double click routes here instead of spawning
			UIContentBrowser::SetVisualPreview(&UIVisualPreview::Show,&UIVisualPreview::ShowFromMemory);
			// device reset: our render target must die before HW.Reset
			g_pOnEditorDeviceResetBegin	= &UIVisualPreview::OnDeviceResetBegin;
			g_pOnEditorDeviceResetEnd	= &UIVisualPreview::OnDeviceResetEnd;
		}
	} s_VisualPreviewHooks;
}
