//---------------------------------------------------------------------------
#include "stdafx.h"
#pragma hdrstop

#include "EThumbnailVisual.h"
#include "EThumbnail.h"
#include "EDX11Utils.h"
#include "..\..\..\XrRender\Private\FBasicVisual.h"
// posing an animated skeleton needs the full interface, not just the forward decl
#include "..\..\..\XrRender\Public\KinematicsAnimated.h"
// library objects (.object) are rendered through CEditableObject::RenderSingle
#include "EditObject.h"

extern BOOL ENGINE_API g_bRendering;

namespace
{
	// Three-quarter shot from above (offset of the camera from the model centre):
	// a flat front view flattens most props into unreadable silhouettes.
	static const Fvector	s_CameraOffset		= {  1.0f,  0.8f, -1.0f };
	static const float		s_FOVDegrees		= 45.f;
	// Neutral dark background, identical for every thumbnail.
	static const u32		s_BackgroundColor	= 0xff2b2b2b;

	//--------------------------------------------------------------------------
	// Temporaries: released whichever way we leave the function.
	//--------------------------------------------------------------------------
	struct SScopedSurface
	{
		IDirect3DSurface9*	s;
							SScopedSurface	()	{ s = 0; }
							~SScopedSurface	()	{ _RELEASE(s); }
	};

	//--------------------------------------------------------------------------
	// We are a guest inside somebody else's frame, so everything we poke gets
	// captured on entry and pushed back on exit. Losing the render target here
	// would take the whole editor down with it.
	//--------------------------------------------------------------------------
	struct SRenderStateGuard
	{
#if defined(USE_DX11)
		// D3D11 has no state blocks. Render target, depth and viewport are the
		// only device state an offscreen pass here overwrites, and that is what
		// SDX11TargetGuard restores; everything else the draw path sets itself.
		SDX11TargetGuard		m_Targets;
		// the D3D9 state block also covered lights and material - the recorded
		// fixed-function block is that state now, and the lights this pass
		// enables must not leak into the scene draw that follows
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

		SRenderStateGuard()
		{
#if defined(USE_DX11)
			m_FF				= EDevice->ff;
#else
			m_Block				= 0;
			m_OldRT				= 0;
			m_OldZB				= 0;
			// D3DSBT_ALL covers render states, samplers, lights and material -
			// the shader passes we are about to run overwrite all of those.
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
			// CRender declares its own ViewBase that SHADOWS the one on
			// IRender_interface, and the culling path (_IsBoxVisible ->
			// CRender::occ_visible) reads the shadowing member. Going through
			// ::Render-> here would write the wrong field and leave the model
			// culled against a stale camera frustum.
			m_OldFrustum		= RImplementation.ViewBase;
		}

		~SRenderStateGuard()
		{
#if defined(USE_DX11)
			EDevice->ff			= m_FF;
#else
			if (m_Block)		m_Block->Apply();
			// render targets are not part of a D3D9 state block - restore by hand,
			// and only then the viewport, since SetRenderTarget resets it
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
			// next draw (the next thumbnail included) re-applies everything
			RCache.OnFrameEnd					();
#if !defined(USE_DX11)
			_RELEASE							(m_Block);
			_RELEASE							(m_OldRT);
			_RELEASE							(m_OldZB);
#endif
		}
	};

	//--------------------------------------------------------------------------
	// The editor draws visuals through the fixed function T&L pipe, so the model
	// needs real lights. Same rig the actor editor preview uses.
	//--------------------------------------------------------------------------
	void SetupThumbnailLights()
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
	void SetupThumbnailStates()
	{
		// states the shader passes do not own themselves: the editor may sit in
		// wireframe/flat/fog mode and that must not leak into a thumbnail.
		// Routed through the device wrapper rather than the raw API, so the DX11
		// build records them for the shader path instead of failing to compile.
		EDevice->SetRS(D3DRS_COLORWRITEENABLE,	D3DCOLORWRITEENABLE_ALPHA|D3DCOLORWRITEENABLE_BLUE|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_RED);
		EDevice->SetRS(D3DRS_FILLMODE,			D3DFILL_SOLID);
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
#if !defined(USE_DX11)
	bool ReadBackThumbnail(IDirect3DSurface9* src, IDirect3DSurface9* sys, U32Vec& out)
	{
		if (FAILED(HW.pDevice->GetRenderTargetData(src,sys)))	return false;

		D3DLOCKED_RECT	D;
		if (FAILED(sys->LockRect(&D,0,D3DLOCK_NOSYSLOCK|D3DLOCK_READONLY)))	return false;

		out.resize	(THUMB_SIZE);
		u32* it		= out.data();
		// Top-down, row 0 first: a D3D9 render target already has row 0 at the
		// top and the uploaders that consume this buffer copy rows straight.
		// EImageThumbnail keeps the opposite convention - its pixels are stored
		// flipped and EImageThumbnail::Update un-flips them while uploading - so
		// this buffer needs a VFlip before it can be handed to that class.
		for (int y=0; y<THUMB_HEIGHT; y++,it+=THUMB_WIDTH)
		{
			const u32* line = (const u32*)(((const u8*)D.pBits)+size_t(D.Pitch)*size_t(y));
			CopyMemory	(it,line,sizeof(u32)*THUMB_WIDTH);
		}
		sys->UnlockRect	();

		// thumbnails are uploaded as X8R8G8B8 - pin alpha so whatever does look
		// at it gets an opaque image instead of the shaders' leftovers
		for (u32 i=0; i<out.size(); i++)	out[i] |= 0xff000000;
		return true;
	}
#endif	//	!USE_DX11

	//--------------------------------------------------------------------------
	// Draws either an engine visual or a library object - everything around the
	// draw call (target, camera, state) is identical, only the subject differs.
	bool RenderVisualToPixels(IRenderVisual* visual, CEditableObject* editable, U32Vec& out)
	{
		if (!visual && !editable)	return false;

		// Animated skeletons with no active blend collapse into a heap: the bone
		// transforms never leave identity. Any cycle at all fixes the pose, so
		// take the first motion the model ships (idle when it has one).
		if (IKinematicsAnimated* KA = visual ? visual->dcast_PKinematicsAnimated() : 0)
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
		// CalculateBones fits the box to the pose we are about to draw -
		// CModelPool::Render would call it anyway, just too late to frame by.
		if (visual)
			if (IKinematics* K = visual->dcast_PKinematics())
				K->CalculateBones(TRUE);

		const Fbox& bb = visual ? visual->getVisData().box : editable->GetBox();
		if ((bb.min.x>bb.max.x)||(bb.min.y>bb.max.y)||(bb.min.z>bb.max.z))	return false;

		Fvector center;	bb.getcenter(center);
		Fvector size;	bb.getsize	(size);
		const float radius = 0.5f*size.magnitude();
		if (radius<EPS_L)	return false;

		SRenderStateGuard	guard;
		const u32 w = THUMB_WIDTH, h = THUMB_HEIGHT;

#if defined(USE_DX11)
		// The whole D3D9 dance is gone: no scene to open or close, no offscreen
		// plain surface, and a readback is legal at any point. That also means
		// this can run inside the ImGui pass safely - the reason the deferred
		// queue exists in the first place.
		SDX11Target	target;
		if (!target.create(w, h))	return false;

		// Invalidate, NOT OnFrameEnd: under D3D11 OnFrameEnd calls ClearState(),
		// which also drops the viewport - and nothing sets one again before the
		// draw, so every triangle was clipped away and the thumbnail came back
		// as bare background. Invalidate only forgets the cache; the device
		// keeps the target and viewport bind() sets right after.
		RCache.Invalidate();
		target.bind();
		target.clear(s_BackgroundColor);
		// and now tell the cache what the device is actually bound to
		RCache.set_RT(target.rtv);
		RCache.set_ZB(target.dsv);

		bool drawn = false;
		{
		{
			// somebody else (ImGui, the previous pass) may have written device
#else
		SScopedSurface		rt, zb, sys;

		if (FAILED(HW.pDevice->CreateRenderTarget(w,h,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&rt.s,0)))
			return false;
		if (FAILED(HW.pDevice->CreateDepthStencilSurface(w,h,HW.Caps.bStencil?D3DFMT_D24S8:D3DFMT_D24X8,D3DMULTISAMPLE_NONE,0,FALSE,&zb.s,0)))
			return false;
		if (FAILED(HW.pDevice->CreateOffscreenPlainSurface(w,h,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&sys.s,NULL)))
			return false;

		// BeginScene answers D3DERR_INVALIDCALL when a scene is already open -
		// the normal case here, since the ImGui pass runs inside the editor's
		// scene. D3D9 wants the scene closed for GetRenderTargetData, so we have
		// to know whether to reopen it afterwards.
		const HRESULT	scene_hr		= HW.pDevice->BeginScene();
		const bool		scene_was_open	= (D3DERR_INVALIDCALL==scene_hr);
		if (!scene_was_open&&FAILED(scene_hr))	return false;

		bool drawn = false;
		// Same reason as the readback below: a throw from the draw itself must not
		// escape with the caller's scene closed.
		try {
		if (SUCCEEDED(HW.pDevice->SetRenderTarget(0,rt.s))&&SUCCEEDED(HW.pDevice->SetDepthStencilSurface(zb.s)))
		{
			D3DVIEWPORT9 vp;
			vp.X = 0; vp.Y = 0; vp.Width = w; vp.Height = h; vp.MinZ = 0.f; vp.MaxZ = 1.f;
			HW.pDevice->SetViewport	(&vp);
			HW.pDevice->Clear		(0,0,D3DCLEAR_ZBUFFER|D3DCLEAR_TARGET|(HW.Caps.bStencil?D3DCLEAR_STENCIL:0),s_BackgroundColor,1.f,0);

			// somebody else (ImGui, the previous pass) may have written device
#endif
			// state behind RCache's back - clear the cache so our draw sets
			// everything up itself instead of trusting stale entries
			// (the D3D11 path already did this above, before binding)
#if !defined(USE_DX11)
			RCache.OnFrameEnd	();
#endif
			SetupThumbnailStates();
			SetupThumbnailLights();

			// frame the bounding sphere of the AABB with a small margin
			Fvector dir	= s_CameraOffset;	dir.normalize();
			const float fov		= deg2rad(s_FOVDegrees);
			const float dist	= radius/_sin(fov*0.5f)*1.12f;
			Fvector eye;	eye.mad(center,dir,dist);

			Fmatrix mV, mP;
			mV.build_camera		(eye,center,Fvector().set(0.f,1.f,0.f));
			// square target -> aspect 1
			mP.build_projection	(fov,1.f,_max(0.01f,dist-radius*1.5f),dist+radius*2.f);

			RCache.set_xform_world	(Fidentity);
			RCache.set_xform_view	(mV);
			RCache.set_xform_project(mP);
			EDevice->mView			= mV;
			EDevice->mProject		= mP;
			EDevice->mFullTransform.mul(mP,mV);
			EDevice->vCameraPosition= eye;
			EDevice->vCameraDirection.sub(center,eye).normalize_safe();
			// CModelPool::Render culls through CRender::ViewBase - without this
			// the model is thrown away before it is ever drawn
			RImplementation.ViewBase.CreateFromMatrix(EDevice->mFullTransform,FRUSTUM_P_ALL);

			// skeletons come out in bind pose: CModelPool::Render calls
			// CalculateBones() for them, no animation update needed
			if (visual)	RImplementation.model_RenderSingle(visual,Fidentity,1.f);
			else		editable->RenderSingle(Fidentity);
			drawn = true;
		}
		}
#if defined(USE_DX11)
		// the guard puts the caller's render target and viewport back; nothing
		// else here needs unwinding
		return drawn ? DX11ReadbackToPixels(target.rt, w, h, out, false) : false;
#else
		catch (...)
		{
			if (guard.m_OldRT)	HW.pDevice->SetRenderTarget(0,guard.m_OldRT);
			HW.pDevice->SetDepthStencilSurface	(guard.m_OldZB);
			if (!scene_was_open)				HW.pDevice->EndScene();
			throw;
		}

		// unbind our surfaces before the readback, then close the scene so
		// GetRenderTargetData is legal
		if (guard.m_OldRT)	HW.pDevice->SetRenderTarget(0,guard.m_OldRT);
		HW.pDevice->SetDepthStencilSurface	(guard.m_OldZB);
		HW.pDevice->EndScene				();

		// Anything below may throw (CHK_DX/R_CHK do), and the ImGui pass this can
		// be called from sits inside a catch(...). An escaping exception would
		// leave the caller's scene closed, and the next frame would die in
		// EDevice->Begin(). Reopening is therefore unconditional.
		bool result = false;
		try		{ result = drawn ? ReadBackThumbnail(rt.s,sys.s,out) : false; }
		catch	(...)
		{
			if (scene_was_open)	HW.pDevice->BeginScene();
			throw;
		}

		// hand the caller back the scene it had open
		if (scene_was_open)	HW.pDevice->BeginScene();

		return result;
#endif
	}

	//--------------------------------------------------------------------------
	// CModelPool::Instance_Create FATALs on visual types the editor does not
	// build (MT_LOD, MT_TREE_*), so anything unknown has to be rejected before
	// the pool ever sees it - a browser sweeps whole folders of unknown files.
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

	// CModelPool::Instance_Load feeds the block straight into r_chunk_safe, which
	// R_ASSERTs on a missing or mis-sized header. Peek it ourselves first.
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
}
//------------------------------------------------------------------------------

ECORE_API bool RenderVisualThumbnail(LPCSTR visual_name, U32Vec& out)
{
	out.clear	();
	if (!visual_name||!visual_name[0])	return false;
	if (!IsDeviceUsable())				return false;

	string_path fn;
	if (!ResolveVisualFile(visual_name,fn))	return false;

	// reject unsupported/broken files before the model pool touches them
	IReader* test = FS.r_open(fn);
	if (!test)							return false;
	u8	 type		= u8(-1);
	bool type_ok	= PeekVisualType(test,type)&&IsSupportedVisualType(type);
	FS.r_close		(test);
	if (!type_ok)						return false;

	// CModelPool::Delete refuses to discard while the engine believes a frame is
	// being rendered; this pass is entirely our own, so clear the flag for it
	const BOOL		saved_rendering	= g_bRendering;
	g_bRendering	= FALSE;

	bool result		= false;
	IRenderVisual* visual = ::Render->model_Create(visual_name);
	if (visual)
	{
		result = RenderVisualToPixels(visual,0,out);
		::Render->model_Delete(visual,TRUE);
	}

	g_bRendering	= saved_rendering;
	if (!result)	out.clear();
	return result;
}
//------------------------------------------------------------------------------

ECORE_API bool RenderVisualThumbnailFromMemory(LPCSTR debug_name, const void* data, u32 size, U32Vec& out)
{
	out.clear	();
	if (!data||(0==size))				return false;
	if (!IsDeviceUsable())				return false;

	IReader reader	((void*)data,int(size));
	u8	 type		= u8(-1);
	if (!PeekVisualType(&reader,type))		return false;
	if (!IsSupportedVisualType(type))
	{
		Msg("!Visual thumbnail: unsupported visual type %d in '%s'.",int(type),(debug_name&&debug_name[0])?debug_name:"<memory>");
		return false;
	}

	// The pool keys models by name and returns the cached copy on a hit, which
	// would silently ignore our block. Generate a private key instead - and keep
	// it dot-free, CModelPool::Create truncates the key at the last '.'.
	static u32	s_uid = 0;
	string_path	pool_key;
	xr_sprintf	(pool_key,sizeof(pool_key),"$thumbnail$%u",++s_uid);

	const BOOL		saved_rendering	= g_bRendering;
	g_bRendering	= FALSE;

	bool result		= false;
	IRenderVisual* visual = ::Render->model_Create(pool_key,&reader);
	if (visual)
	{
		result = RenderVisualToPixels(visual,0,out);
		::Render->model_Delete(visual,TRUE);
	}

	g_bRendering	= saved_rendering;
	if (!result)	out.clear();
	return result;
}
ECORE_API bool RenderObjectThumbnail(LPCSTR object_name, U32Vec& out)
{
	out.clear();
	if (!object_name||!object_name[0])	return false;
	if (!IsDeviceUsable())				return false;

	string_path fn;
	FS.update_path(fn,_objects_,EFS.ChangeFileExt(object_name,".object").c_str());
	if (!FS.exist(fn))					return false;

	// a private copy: the shared library instance must not be touched, and the
	// object is only alive for this one draw
	CEditableObject* obj = xr_new<CEditableObject>(object_name);
	bool result = false;
	if (obj->Load(fn))
		result = RenderVisualToPixels(0,obj,out);
	xr_delete(obj);

	if (!result)	out.clear();
	return result;
}
//------------------------------------------------------------------------------
// deferred queue
//------------------------------------------------------------------------------
namespace
{
	enum EThumbSource { tsVisualName, tsVisualBytes, tsEditableObject };

	struct SThumbRequest
	{
		xr_string		key;
		xr_string		name;		// visual or object name, by source
		xr_vector<u8>	bytes;
		EThumbSource	source;
		bool			done;
		bool			failed;
		U32Vec			pixels;
		SThumbRequest() : source(tsVisualName), done(false), failed(false) {}
	};

	static xr_vector<SThumbRequest>	s_Requests;

	static SThumbRequest* FindRequest(LPCSTR key)
	{
		for (u32 i = 0; i < s_Requests.size(); ++i)
			if (s_Requests[i].key == key) return &s_Requests[i];
		return 0;
	}
}

ECORE_API void QueueVisualThumbnail(LPCSTR key, LPCSTR visual_name)
{
	if (!key||!key[0]||!visual_name||!visual_name[0])	return;
	if (FindRequest(key))								return;
	s_Requests.push_back(SThumbRequest());
	SThumbRequest& r = s_Requests.back();
	r.key		= key;
	r.name		= visual_name;
	r.source	= tsVisualName;
}

ECORE_API void QueueObjectThumbnail(LPCSTR key, LPCSTR object_name)
{
	if (!key||!key[0]||!object_name||!object_name[0])	return;
	if (FindRequest(key))								return;
	s_Requests.push_back(SThumbRequest());
	SThumbRequest& r = s_Requests.back();
	r.key		= key;
	r.name		= object_name;
	r.source	= tsEditableObject;
}

ECORE_API void QueueVisualThumbnailFromMemory(LPCSTR key, const void* data, u32 size)
{
	if (!key||!key[0]||!data||(0==size))	return;
	if (FindRequest(key))					return;
	s_Requests.push_back(SThumbRequest());
	SThumbRequest& r = s_Requests.back();
	r.key		= key;
	r.source	= tsVisualBytes;
	r.bytes.assign((const u8*)data,(const u8*)data+size);
}

ECORE_API bool TakeVisualThumbnail(LPCSTR key, U32Vec& out, bool& failed)
{
	out.clear();
	failed = false;
	for (u32 i = 0; i < s_Requests.size(); ++i)
	{
		if (s_Requests[i].key != key)	continue;
		if (!s_Requests[i].done)		return false;	// still waiting its turn
		out		= s_Requests[i].pixels;
		failed	= s_Requests[i].failed;
		s_Requests.erase(s_Requests.begin()+i);
		return true;
	}
	return false;
}

ECORE_API void FlushVisualThumbnailQueue(u32 max_requests)
{
	if (s_Requests.empty()||!IsDeviceUsable())	return;

	u32 done = 0;
	for (u32 i = 0; i < s_Requests.size() && done < max_requests; ++i)
	{
		SThumbRequest& r = s_Requests[i];
		if (r.done)	continue;

		// One bad model must not take the editor down with it: the render path
		// can throw, and here there is no frame in flight to corrupt.
		bool ok = false;
		try
		{
			switch (r.source)
			{
			case tsVisualBytes:
				ok = RenderVisualThumbnailFromMemory(r.key.c_str(),r.bytes.data(),u32(r.bytes.size()),r.pixels);
				break;
			case tsEditableObject:
				ok = RenderObjectThumbnail(r.name.c_str(),r.pixels);
				break;
			default:
				ok = RenderVisualThumbnail(r.name.c_str(),r.pixels);
				break;
			}
		}
		catch (...)
		{
			Msg("!Visual thumbnail: render failed for '%s'.",r.key.c_str());
			ok = false;
		}

		r.done		= true;
		r.failed	= !ok;
		r.bytes.clear();	// the source block is not needed once it is drawn
		++done;
	}
}

ECORE_API void ClearVisualThumbnailQueue()
{
	s_Requests.clear();
}
//------------------------------------------------------------------------------
