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
#include "EditorProject.h"
#include "EditorGameContent.h"

extern BOOL ENGINE_API g_bRendering;

namespace
{
	// Three-quarter shot from above (offset of the camera from the model centre):
	// a flat front view flattens most props into unreadable silhouettes.
	static const Fvector	s_CameraOffset		= {  1.0f,  0.8f, -1.0f };
	static const float		s_FOVDegrees		= 45.f;
	// Neutral dark background, identical for every thumbnail.
	static const u32		s_BackgroundColor	= 0xff2b2b2b;
}

// A queued request keeps the source block in memory until it is drawn, and a
// search over the linked game can put hundreds of tiles on screen at once. Both
// caps are per-request/total guards against a folder of big meshes eating the
// address space - a model too big for a 128px picture simply does not get one.
static const u32			kMaxThumbSourceBytes	= 16u*1024u*1024u;
static const u32			kMaxThumbQueueBytes		= 128u*1024u*1024u;

namespace
{

	//--------------------------------------------------------------------------
	// Temporaries: released whichever way we leave the function.
	// D3D9 only - SDX11Target owns its own lifetime.
	//--------------------------------------------------------------------------
#if !defined(USE_DX11)
	struct SScopedSurface
	{
		IDirect3DSurface9*	s;
							SScopedSurface	()	{ s = 0; }
							~SScopedSurface	()	{ _RELEASE(s); }
	};
#endif

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
		// LL_MotionsSlotCount()==0 means the motions never loaded - the model is
		// half-built and its blend pool is empty, so posing it would fault. The
		// from-memory path avoids ever getting here (animated visuals are loaded
		// rigid), this is the guard for anything that still slips through.
		if (IKinematicsAnimated* KA = visual ? visual->dcast_PKinematicsAnimated() : 0;
			KA && KA->LL_MotionsSlotCount())
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
		if (!drawn || !DX11ReadbackToPixels(target.rt, w, h, out, false))
			return false;
		// Pin alpha, exactly as the D3D9 branch below does. The colour write mask
		// leaves alpha enabled, so the background clear and any alpha-tested
		// surface put non-opaque values in here - and PixelsToTexture uploads the
		// buffer verbatim, which is what made browser tiles look washed out.
		for (u32 i = 0; i < out.size(); ++i)	out[i] |= 0xff000000;
		return true;
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

	// Byte offset of ogf_header::type inside the block, -1 when there is no
	// header chunk. type is the second byte of the chunk payload, right after
	// format_version.
	int OgfTypeOffset(IReader* data)
	{
		if (!data)								return -1;
		const int at = (sizeof(ogf_header)==data->find_chunk(OGF_HEADER)) ? data->tell() : -1;
		data->rewind();
		return (at<0) ? -1 : (at+1);
	}

	//--------------------------------------------------------------------------
	// A thumbnail is a STATIC picture, so it is taken of the bind pose and needs
	// no animation at all - but MT_SKELETON_ANIM makes the model pool build a
	// CKinematicsAnimated, which loads the model's external .omf motion files.
	// For a visual out of the linked game those live in the game's archives,
	// which the editor's own FS cannot see, and the SDK's editor build "handles"
	// the miss by returning from the middle of CKinematicsAnimated::Load
	// (SkeletonAnimated.cpp, the REDITOR branch): no partition, no motions, no
	// blend pool. Rendering that half-built object then takes the editor down -
	// which is what a search in DARF Content did, by asking for a few hundred
	// actor meshes at once.
	//
	// Loading it as MT_SKELETON_RIGID gives a plain CKinematics: same skeleton,
	// same geometry, motions never touched. The picture is identical.
	bool NeuterAnimatedVisual(const void* data, u32 size, u8 type, xr_vector<u8>& copy, const void*& block)
	{
		block = data;
		if (MT_SKELETON_ANIM!=type)	return true;

		IReader probe((void*)data,int(size));
		const int off = OgfTypeOffset(&probe);
		if ((off<0)||(u32(off)>=size))	return false;

		copy.assign((const u8*)data,(const u8*)data+size);
		copy[off]	= MT_SKELETON_RIGID;
		block		= copy.data();
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

// Forward: the by-name path hands animated visuals to the from-memory one.
static bool RenderVisualThumbnailFromMemory_Impl(LPCSTR debug_name, const void* data, u32 size, U32Vec& out);

static bool RenderVisualThumbnail_Impl(LPCSTR visual_name, U32Vec& out)
{
	string_path fn;
	if (!ResolveVisualFile(visual_name,fn))	return false;

	// reject unsupported/broken files before the model pool touches them
	IReader* test = FS.r_open(fn);
	if (!test)							return false;
	u8	 type		= u8(-1);
	bool type_ok	= PeekVisualType(test,type)&&IsSupportedVisualType(type);
	const bool animated = type_ok&&(MT_SKELETON_ANIM==type);
	// An animated skeleton goes through the from-memory path, which loads it as
	// a rigid one: the model pool has no way to say "skip the motions", and a
	// motion file it cannot resolve leaves the visual half-built (see
	// NeuterAnimatedVisual). Cheap enough - a mesh is a few MB at worst.
	xr_vector<u8> bytes;
	if (animated)
	{
		const u32 sz = test->length();
		if (sz&&(sz<=kMaxThumbSourceBytes))
		{
			bytes.resize(sz);
			test->seek(0);
			test->r(bytes.data(),int(sz));
		}
	}
	FS.r_close		(test);
	if (!type_ok)						return false;
	if (animated)
	{
		if (bytes.empty())				return false;
		return RenderVisualThumbnailFromMemory_Impl(visual_name,bytes.data(),u32(bytes.size()),out);
	}

	// CModelPool::Delete refuses to discard while the engine believes a frame is
	// being rendered; this pass is entirely our own, so clear the flag for it
	const BOOL		saved_rendering	= g_bRendering;
	g_bRendering	= FALSE;

	// The loaders R_ASSERT on every malformation, and a file out of a foreign
	// game install may legitimately be one this SDK cannot parse. Inside the
	// soft scope those asserts throw - caught right here, a failed thumbnail.
	bool result		= false;
	IRenderVisual*	visual = 0;
	{
		xrDebug::soft_assert_scope soft;
		try
		{
			visual = ::Render->model_Create(visual_name);
			if (visual)	result = RenderVisualToPixels(visual,0,out);
		}
		catch (...)
		{
			Msg("!Visual thumbnail: '%s' failed to load - skipped.",visual_name);
			result = false;
		}
		if (visual)
		{
			try { ::Render->model_Delete(visual,TRUE); } catch (...) {}
		}
	}

	g_bRendering	= saved_rendering;
	if (!result)	out.clear();
	return result;
}
//------------------------------------------------------------------------------

static bool RenderVisualThumbnailFromMemory_Impl(LPCSTR debug_name, const void* data, u32 size, U32Vec& out)
{
	IReader reader	((void*)data,int(size));
	u8	 type		= u8(-1);
	if (!PeekVisualType(&reader,type))		return false;
	if (!IsSupportedVisualType(type))
	{
		Msg("!Visual thumbnail: unsupported visual type %d in '%s'.",int(type),(debug_name&&debug_name[0])?debug_name:"<memory>");
		return false;
	}

	// animated skeletons are loaded as rigid ones - see NeuterAnimatedVisual
	xr_vector<u8>	copy;
	const void*		block = data;
	if (!NeuterAnimatedVisual(data,size,type,copy,block))
	{
		Msg("!Visual thumbnail: '%s' has no readable OGF header.",(debug_name&&debug_name[0])?debug_name:"<memory>");
		return false;
	}
	IReader	use((void*)block,int(size));

	// The pool keys models by name and returns the cached copy on a hit, which
	// would silently ignore our block. Generate a private key instead - and keep
	// it dot-free, CModelPool::Create truncates the key at the last '.'.
	static u32	s_uid = 0;
	string_path	pool_key;
	xr_sprintf	(pool_key,sizeof(pool_key),"$thumbnail$%u",++s_uid);

	const BOOL		saved_rendering	= g_bRendering;
	g_bRendering	= FALSE;

	bool result		= false;
	IRenderVisual*	visual = 0;
	{
		xrDebug::soft_assert_scope soft;
		try
		{
			visual = ::Render->model_Create(pool_key,&use);
			if (visual)	result = RenderVisualToPixels(visual,0,out);
		}
		catch (...)
		{
			Msg("!Visual thumbnail: '%s' failed to load - skipped.",
				(debug_name&&debug_name[0])?debug_name:"<memory>");
			result = false;
		}
		if (visual)
		{
			try { ::Render->model_Delete(visual,TRUE); } catch (...) {}
		}
	}

	g_bRendering	= saved_rendering;
	if (!result)	out.clear();
	return result;
}
static bool RenderObjectThumbnail_Impl(LPCSTR object_name, U32Vec& out)
{
	string_path fn;
	FS.update_path(fn,_objects_,EFS.ChangeFileExt(object_name,".object").c_str());
	if (!FS.exist(fn))					return false;

	// a private copy: the shared library instance must not be touched, and the
	// object is only alive for this one draw
	CEditableObject* obj = xr_new<CEditableObject>(object_name);
	bool result = false;
	{
		xrDebug::soft_assert_scope soft;
		try
		{
			if (obj->Load(fn))
				result = RenderVisualToPixels(0,obj,out);
		}
		catch (...)
		{
			Msg("!Object thumbnail: '%s' failed to load - skipped.",object_name);
			result = false;
		}
	}
	xr_delete(obj);

	return result;
}
//------------------------------------------------------------------------------

static bool RenderObjectThumbnailFromMemory_Impl(LPCSTR debug_name, const void* data, u32 size, U32Vec& out)
{
	// A project's own .object is not in $objects$ and, living outside the SDK
	// root, is not registered in the editor FS at all - so the by-name path
	// above can never reach it. The bytes come straight off the disk instead.
	//
	// Load(IReader&) reads the object BODY, not the file: the file-based
	// overload opens EOBJ_CHUNK_OBJECT_BODY first, and feeding it the whole
	// file asserts on the missing version chunk.
	IReader reader	((void*)data,int(size));
	IReader* body	= reader.open_chunk(EOBJ_CHUNK_OBJECT_BODY);
	if (!body)
	{
		Msg("!Object thumbnail: '%s' is not an .object file.",(debug_name&&debug_name[0])?debug_name:"<memory>");
		return false;
	}

	CEditableObject* obj = xr_new<CEditableObject>((debug_name&&debug_name[0])?debug_name:"$memory$");
	bool result = false;
	{
		xrDebug::soft_assert_scope soft;
		try
		{
			if (obj->Load(*body))
				result = RenderVisualToPixels(0,obj,out);
		}
		catch (...)
		{
			Msg("!Object thumbnail: '%s' failed to load - skipped.",
				(debug_name&&debug_name[0])?debug_name:"<memory>");
			result = false;
		}
	}
	xr_delete(obj);
	body->close();

	return result;
}

//------------------------------------------------------------------------------
// Public entries. The impls above turn ASSERTS into failed thumbnails through
// the soft-assert scope; this layer stops the faults asserts cannot see - a
// real access violation inside a parser fed garbage. __except keeps it from
// the process-wide filter; the skipped C++ destructors are the accepted price,
// the render state is re-applied wholesale next frame anyway.
//------------------------------------------------------------------------------
namespace
{
	struct SThumbCall
	{
		int			kind;	// 0 visual name, 1 visual bytes, 2 object name, 3 object bytes
		LPCSTR		name;
		const void*	data;
		u32			size;
		U32Vec*		out;
		bool		ok;
	};

	void ThumbCallBody(SThumbCall* c)
	{
		switch (c->kind)
		{
		case 0:	c->ok = RenderVisualThumbnail_Impl			(c->name,*c->out);					break;
		case 1:	c->ok = RenderVisualThumbnailFromMemory_Impl(c->name,c->data,c->size,*c->out);	break;
		case 2:	c->ok = RenderObjectThumbnail_Impl			(c->name,*c->out);					break;
		case 3:	c->ok = RenderObjectThumbnailFromMemory_Impl(c->name,c->data,c->size,*c->out);	break;
		}
	}

	// nothing with a destructor may live in a __try frame (C2712), so this
	// function holds nothing at all
	bool ThumbCallSEH(SThumbCall* c)
	{
		__try			{ ThumbCallBody(c); return true; }
		__except		(EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool ThumbCall(int kind, LPCSTR name, const void* data, u32 size, U32Vec& out)
	{
		out.clear();
		if (!IsDeviceUsable())	return false;

		SThumbCall c;
		c.kind = kind; c.name = name; c.data = data; c.size = size; c.out = &out; c.ok = false;
		if (!ThumbCallSEH(&c))
		{
			Msg("!Thumbnail: '%s' FAULTED while loading - skipped.",(name&&name[0])?name:"<memory>");
			c.ok = false;
		}
		if (!c.ok)	out.clear();
		return c.ok;
	}
}

ECORE_API bool RenderVisualThumbnail(LPCSTR visual_name, U32Vec& out)
{
	if (!visual_name||!visual_name[0])	{ out.clear(); return false; }
	return ThumbCall(0,visual_name,0,0,out);
}

ECORE_API bool RenderVisualThumbnailFromMemory(LPCSTR debug_name, const void* data, u32 size, U32Vec& out)
{
	if (!data||(0==size))				{ out.clear(); return false; }
	return ThumbCall(1,debug_name,data,size,out);
}

ECORE_API bool RenderObjectThumbnail(LPCSTR object_name, U32Vec& out)
{
	if (!object_name||!object_name[0])	{ out.clear(); return false; }
	return ThumbCall(2,object_name,0,0,out);
}

ECORE_API bool RenderObjectThumbnailFromMemory(LPCSTR debug_name, const void* data, u32 size, U32Vec& out)
{
	if (!data||(0==size))				{ out.clear(); return false; }
	return ThumbCall(3,debug_name,data,size,out);
}

//------------------------------------------------------------------------------
// scene visuals (see the header): same loading rules as a thumbnail, but the
// visual is handed to the caller alive instead of being drawn once and dropped
//------------------------------------------------------------------------------
namespace
{
	IRenderVisual* CreateSceneVisual_Impl(LPCSTR debug_name, const void* data, u32 size)
	{
		IReader	reader((void*)data,int(size));
		u8		type = u8(-1);
		if (!PeekVisualType(&reader,type))		return 0;
		if (!IsSupportedVisualType(type))
		{
			Msg("!Scene visual: unsupported visual type %d in '%s'.",int(type),
				(debug_name&&debug_name[0])?debug_name:"<memory>");
			return 0;
		}

		xr_vector<u8>	copy;
		const void*		block = data;
		if (!NeuterAnimatedVisual(data,size,type,copy,block))
		{
			Msg("!Scene visual: '%s' has no readable OGF header.",(debug_name&&debug_name[0])?debug_name:"<memory>");
			return 0;
		}
		IReader	use((void*)block,int(size));

		// unique key: the pool would otherwise hand back a cached model under a
		// name that has nothing to do with these bytes
		static u32	s_uid = 0;
		string_path	pool_key;
		xr_sprintf	(pool_key,sizeof(pool_key),"$scenevisual$%u",++s_uid);

		IRenderVisual* visual = 0;
		{
			xrDebug::soft_assert_scope soft;
			try		{ visual = ::Render->model_Create(pool_key,&use); }
			catch (...)
			{
				Msg("!Scene visual: '%s' failed to load - skipped.",(debug_name&&debug_name[0])?debug_name:"<memory>");
				visual = 0;
			}
		}
		return visual;
	}

	struct SVisualCall { LPCSTR name; const void* data; u32 size; IRenderVisual* result; };
	void VisualCallBody(SVisualCall* c)	{ c->result = CreateSceneVisual_Impl(c->name,c->data,c->size); }
	bool VisualCallSEH(SVisualCall* c)
	{
		__try		{ VisualCallBody(c); return true; }
		__except	(EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
}

ECORE_API IRenderVisual* CreateSceneVisualFromMemory(LPCSTR debug_name, const void* data, u32 size)
{
	if (!data||(0==size)||!IsDeviceUsable())	return 0;

	SVisualCall c; c.name = debug_name; c.data = data; c.size = size; c.result = 0;
	if (!VisualCallSEH(&c))
	{
		Msg("!Scene visual: '%s' FAULTED while loading - skipped.",(debug_name&&debug_name[0])?debug_name:"<memory>");
		return 0;
	}
	return c.result;
}

ECORE_API IRenderVisual* CreateSceneVisual(LPCSTR path)
{
	if (!path||!path[0])	return 0;

	xr_vector<u8> bytes;

	// 1. the project (the browser hands out project-relative paths, and the
	//    project root is not registered in the editor FS at all)
	if (EditorProject::Active())
	{
		string_path full;
		sprintf_s(full,"%s\\%s",EditorProject::Root(),path);
		if (HANDLE h = ::CreateFileA(full,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
			INVALID_HANDLE_VALUE != h)
		{
			LARGE_INTEGER sz;
			if (::GetFileSizeEx(h,&sz) && sz.QuadPart > 0 && sz.QuadPart < LONGLONG(kMaxThumbSourceBytes))
			{
				bytes.resize(size_t(sz.QuadPart));
				DWORD read = 0;
				if (!::ReadFile(h,bytes.data(),DWORD(bytes.size()),&read,NULL) || read != bytes.size())
					bytes.clear();
			}
			::CloseHandle(h);
		}
	}

	// 2. the linked game install (archives + its loose gamedata); mounts on
	//    demand - a scene with game meshes may load before the browser ever
	//    opened the DARF tab
	if (bytes.empty())
	{
		xr_string mount_err;
		const int idx = EditorGameContent::EnsureMounted(mount_err) ? EditorGameContent::Find(path) : -1;
		if (idx >= 0)
		{
			u32 sz = 0;
			u8* raw = EditorGameContent::ReadBytes(idx,sz);
			if (raw && sz)	bytes.assign(raw,raw+sz);
			EditorGameContent::FreeBytes(raw);
		}
	}

	// 3. the editor's own FS ($level$ / $game_meshes$), by name
	if (bytes.empty())
	{
		string_path fn;
		if (ResolveVisualFile(path,fn))
			if (IReader* r = FS.r_open(fn))
			{
				const u32 sz = r->length();
				if (sz && sz <= kMaxThumbSourceBytes)
				{
					bytes.resize(sz);
					r->seek(0);
					r->r(bytes.data(),int(sz));
				}
				FS.r_close(r);
			}
	}

	if (bytes.empty())
	{
		Msg("!Scene visual: '%s' - no readable .ogf in the project, the linked game or the editor tree.",path);
		return 0;
	}
	return CreateSceneVisualFromMemory(path,bytes.data(),u32(bytes.size()));
}

//------------------------------------------------------------------------------
// deferred queue
//------------------------------------------------------------------------------
namespace
{
	enum EThumbSource { tsVisualName, tsVisualBytes, tsEditableObject, tsEditableObjectBytes };

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

	using TRequestMap = xr_flat_hash_map<xr_string, SThumbRequest>;
	static TRequestMap			s_Requests;
	static xr_deque<xr_string>	s_PendingRequests;
	static u32					s_QueuedBytes = 0;

	// true when the block may be queued; the caller drops the request otherwise
	// and the tile falls back to a plain name button, exactly like a failed
	// render does.
	static bool AcceptQueuedBytes(LPCSTR key, u32 size)
	{
		if (size > kMaxThumbSourceBytes)
		{
			Msg("~Visual thumbnail: '%s' is %u MB - too big to preview.",key,size/(1024u*1024u));
			return false;
		}
		if (s_QueuedBytes + size > kMaxThumbQueueBytes)	return false;
		s_QueuedBytes += size;
		return true;
	}
}

ECORE_API bool HasVisualThumbnailRequest(LPCSTR key)
{
	return key && key[0] && s_Requests.find(key) != s_Requests.end();
}

ECORE_API void QueueVisualThumbnail(LPCSTR key, LPCSTR visual_name)
{
	if (!key||!key[0]||!visual_name||!visual_name[0])	return;
	if (HasVisualThumbnailRequest(key))					return;
	SThumbRequest& r = s_Requests[key];
	r.key		= key;
	r.name		= visual_name;
	r.source	= tsVisualName;
	s_PendingRequests.push_back(key);
}

ECORE_API void QueueObjectThumbnail(LPCSTR key, LPCSTR object_name)
{
	if (!key||!key[0]||!object_name||!object_name[0])	return;
	if (HasVisualThumbnailRequest(key))					return;
	SThumbRequest& r = s_Requests[key];
	r.key		= key;
	r.name		= object_name;
	r.source	= tsEditableObject;
	s_PendingRequests.push_back(key);
}

ECORE_API void QueueVisualThumbnailFromMemory(LPCSTR key, const void* data, u32 size)
{
	if (!key||!key[0]||!data||(0==size))	return;
	if (HasVisualThumbnailRequest(key))		return;
	if (!AcceptQueuedBytes(key,size))		return;
	SThumbRequest& r = s_Requests[key];
	r.key		= key;
	r.name		= key;		// logging only
	r.source	= tsVisualBytes;
	r.bytes.assign((const u8*)data,(const u8*)data+size);
	s_PendingRequests.push_back(key);
}

ECORE_API void QueueObjectThumbnailFromMemory(LPCSTR key, const void* data, u32 size)
{
	if (!key||!key[0]||!data||(0==size))	return;
	if (HasVisualThumbnailRequest(key))		return;
	if (!AcceptQueuedBytes(key,size))		return;
	SThumbRequest& r = s_Requests[key];
	r.key		= key;
	r.name		= key;		// logging only
	r.source	= tsEditableObjectBytes;
	r.bytes.assign((const u8*)data,(const u8*)data+size);
	s_PendingRequests.push_back(key);
}

ECORE_API bool TakeVisualThumbnail(LPCSTR key, U32Vec& out, bool& failed)
{
	out.clear();
	failed = false;
	if (!key || !key[0]) return false;
	const TRequestMap::iterator it = s_Requests.find(key);
	if (it == s_Requests.end() || !it->second.done) return false;

	out.swap(it->second.pixels);
	failed	= it->second.failed;
	// A completed request normally released its source block in Flush. Keep the
	// accounting defensive so a future early-completion path cannot leak budget.
	if (const u32 held = u32(it->second.bytes.size()))
		s_QueuedBytes = (s_QueuedBytes>held) ? (s_QueuedBytes-held) : 0;
	s_Requests.erase(it);
	return true;
}

ECORE_API void FlushVisualThumbnailQueue(u32 max_requests)
{
	if (s_PendingRequests.empty()||!IsDeviceUsable())	return;

	u32 done = 0;
	while (!s_PendingRequests.empty() && done < max_requests)
	{
		const xr_string key = s_PendingRequests.front();
		s_PendingRequests.pop_front();
		const TRequestMap::iterator it = s_Requests.find(key);
		if (it == s_Requests.end()) continue;
		SThumbRequest& r = it->second;

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
			case tsEditableObjectBytes:
				ok = RenderObjectThumbnailFromMemory(r.name.c_str(),r.bytes.data(),u32(r.bytes.size()),r.pixels);
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
		// the source block is not needed once it is drawn, and the budget it
		// took has to go back or the queue silently stops accepting work
		if (const u32 held = u32(r.bytes.size()))
		{
			s_QueuedBytes = (s_QueuedBytes>held) ? (s_QueuedBytes-held) : 0;
			r.bytes.clear();
			r.bytes.shrink_to_fit();
		}
		++done;
	}
}

ECORE_API void ClearVisualThumbnailQueue()
{
	s_Requests.clear();
	s_PendingRequests.clear();
	s_QueuedBytes = 0;
}
//------------------------------------------------------------------------------
