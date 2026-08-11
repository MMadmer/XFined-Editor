#ifndef	COMMON_H
#define COMMON_H

#include "shared\common.h"

uniform float4		L_dynamic_props;	// per object, xyz=sun,w=hemi
uniform float4		L_dynamic_color;	// dynamic light color (rgb1)	- spot/point
uniform float4		L_dynamic_pos;		// dynamic light pos+1/range(w) - spot/point
uniform float4x4 	L_dynamic_xform;

uniform float4x4	m_plmap_xform;
uniform float4 		m_plmap_clamp	[2];	// 0.w = factor

//////////////////////////////////////////////////////////////////////////////////////////
// Resources. The engine looks these up by name through shader reflection
// (r_dx10Texture / r_dx10Sampler), so the names must stay exactly as they are.
Texture2D 		s_base;		//	smp_base
TextureCube		s_env;		//	smp_rtlinear
Texture2D 		s_lmap;		//
Texture2D 		s_hemi;		//
Texture2D 		s_att;		//
Texture2D 		s_detail;	//	smp_base

sampler 	smp_nofilter;	//	D3DTADDRESS_CLAMP,	POINT,			NONE,	POINT
sampler 	smp_rtlinear;	//	D3DTADDRESS_CLAMP,	LINEAR,			NONE,	LINEAR
sampler 	smp_linear;		//	D3DTADDRESS_WRAP,	LINEAR,			LINEAR,	LINEAR
sampler 	smp_base;		//	D3DTADDRESS_WRAP,	ANISOTROPIC,	LINEAR,	ANISOTROPIC

//////////////////////////////////////////////////////////////////////////////////////////
float  	calc_fogging 	(float4 w_pos)	{ return dot(w_pos,fog_plane); 	}
float2 	calc_detail 	(float3 w_pos)	{
	float  	dtl	= distance(w_pos,eye_position)*dt_params.w;
		dtl	= min(dtl*dtl, 1);
	float  	dt_mul	= 1  - dtl;	// dt*  [1 ..  0 ]
	float  	dt_add	= .5 * dtl;	// dt+	[0 .. 0.5]
	return	float2	(dt_mul,dt_add);
}
float3 	calc_reflection	(float3 pos_w, float3 norm_w)
{
    return reflect(normalize(pos_w-eye_position), norm_w);
}
float4	calc_spot 	(out float4 tc_lmap, out float2 tc_att, float4 w_pos, float3 w_norm)	{
	float4 	s_pos	= mul	(L_dynamic_xform, w_pos);
	tc_lmap		= s_pos.xyww;			// projected in ps/ttf
	tc_att 		= s_pos.z;			// z=distance * (1/range)
	float3 	L_dir_n = normalize	(w_pos.xyz - L_dynamic_pos.xyz);
	float 	L_scale	= dot(w_norm,-L_dir_n);
	return 	L_dynamic_color*L_scale*saturate(calc_fogging(w_pos));
}
float4	calc_point 	(out float2 tc_att0, out float2 tc_att1, float4 w_pos, float3 w_norm)	{
	float3 	L_dir_n = normalize	(w_pos.xyz - L_dynamic_pos.xyz);
	float 	L_scale	= dot		(w_norm,-L_dir_n);
	float3	L_tc 	= (w_pos.xyz - L_dynamic_pos.xyz) * L_dynamic_pos.w + .5f;	// tc coords
	tc_att0		= L_tc.xz;
	tc_att1		= L_tc.xy;
	return 	L_dynamic_color*L_scale*saturate(calc_fogging(w_pos));
}
float3	calc_sun		(float3 norm_w)	{ return L_sun_color*max(dot((norm_w),-L_sun_dir_w),0); 		}
float3 	calc_model_hemi 	(float3 norm_w)	{ return (norm_w.y*0.5+0.5)*L_dynamic_props.w*L_hemi_color.rgb; 	}
float3	calc_model_lq_lighting	(float3 norm_w) { return calc_model_hemi(norm_w) + L_ambient.rgb + L_dynamic_props.xyz*calc_sun(norm_w); 	}
float3 	_calc_model_hemi 	(float3 norm_w)	{ return max(0,norm_w.y)*.2*L_hemi_color.rgb; 				}
float3	_calc_model_lq_lighting	(float3 norm_w) { return calc_model_hemi(norm_w) + L_ambient.rgb + .5*calc_sun(norm_w); 	}
float4 	calc_model_lmap 	(float3 pos_w)	{
	float3  pos_wc	= clamp		(pos_w,m_plmap_clamp[0].xyz,m_plmap_clamp[1].xyz);	// clamp to BBox
	float4 	pos_w4c	= float4	(pos_wc,1);
	float4 	plmap 	= mul		(m_plmap_xform,pos_w4c);				// calc plmap tc
	return  plmap.xyww;
}

struct 	v_lmap
{
	float4 	P	: POSITION;			// (float,float,float,1)
	float4	N	: NORMAL;			// (nx,ny,nz,hemi occlusion)
	float4 	T	: TANGENT;
	float4 	B	: BINORMAL;
	float2 	uv0	: TEXCOORD0;		// (base)
	float2	uv1	: TEXCOORD1;		// (lmap/compressed)
};
struct	v_vert
{
	float4 	P		: POSITION;		// (float,float,float,1)
	float4	N		: NORMAL;		// (nx,ny,nz,hemi occlusion)
	float4 	T		: TANGENT;
	float4 	B		: BINORMAL;
	float4	color	: COLOR0;		// (r,g,b,dir-occlusion)
	float2 	uv		: TEXCOORD0;	// (u0,v0)
};
struct 	v_model
{
	float4 	pos	: POSITION;	// (float,float,float,1)
	float3	norm	: NORMAL;	// (nx,ny,nz)
	float3	T	: TANGENT;	// (nx,ny,nz)
	float3	B	: BINORMAL;	// (nx,ny,nz)
	float2	tc	: TEXCOORD0;	// (u,v)
#ifdef SKIN_COLOR
	float3 	rgb_tint;
#endif
};
// The editor pushes plain FVF geometry through the same shader as real OGF
// visuals. D3D11 rejects an input layout that is missing a semantic the shader
// consumes, so the unskinned entry point asks only for what every editor mesh
// actually carries: position, normal, uv. Tangent frames exist solely in the
// skinned OGF declarations - demanding them here is what used to kill the
// layout. Extra streams in the layout are fine; missing ones are not.
struct	v_model_min
{
	float4 	pos	: POSITION;	// (float,float,float,1)
	float3	norm	: NORMAL;	// (nx,ny,nz)
	float2	tc	: TEXCOORD0;	// (u,v)
};

struct	v_detail
{
	float4 	pos	: POSITION;	// (float,float,float,1)
	int4 	misc	: TEXCOORD0;	// (u(Q),v(Q),frac,matrix-id)
};

//////////////////////////////////////////////////////////////////////////////////////////
// Fixed-function lighting emulation. The engine records the editor's SetLight/
// LightEnable/SetRS(AMBIENT) calls and feeds them here on every set_Element -
// see EDevice_PushFFConstants. Zero enabled lights = the unlit path, which is
// how the scene viewport always rendered; previews and thumbnails enable a rig
// of directional lights, same as they did through D3D9's T&L.
uniform float4		ffp_ambient;		// rgb, a=1
uniform float4		ffp_params;			// x = enabled light count
uniform float4		ffp_light_dir	[4];	// xyz = direction (world)
uniform float4		ffp_light_color	[4];	// rgb = diffuse

float3	calc_ffp_lighting	(float3 norm_w)
{
	int count	= int(ffp_params.x);
	if (count <= 0)
		return float3(1,1,1);

	float3 acc	= ffp_ambient.rgb;
	for (int i=0; i<4; i++)
	{
		if (i >= count)	break;
		acc	+= ffp_light_color[i].rgb * max(0, dot(norm_w, -normalize(ffp_light_dir[i].xyz)));
	}
	return acc;
}

//////////////////////////////////////////////////////////////////////////////////////////
// Pre-transformed (screen-space) geometry - FVF::F_TL / FVF::F_TL0uv.
// D3D11 has no fixed-function T&L, so these now need a real vertex shader.
uniform float4		screen_res;		// x=Width, y=Height, zw = 1/resolution

struct	v_TL_positiont
{
	float4	P		: POSITIONT;
	float2	Tex0	: TEXCOORD0;
	float4	Color	: COLOR;
};
struct	v_TL
{
	float4	P		: POSITION;
	float2	Tex0	: TEXCOORD0;
	float4	Color	: COLOR;
};
struct	v2p_TL
{
	float2 	Tex0	: TEXCOORD0;
	float4	Color	: COLOR;
	float4 	HPos	: SV_Position;
};
struct	p_TL
{
	float2 	Tex0	: TEXCOORD0;
	float4	Color	: COLOR;
};

struct	v_TL0uv_positiont
{
	float4	P		: POSITIONT;
	float4	Color	: COLOR;
};
struct	v2p_TL0uv
{
	float4	Color	: COLOR;
	float4 	HPos	: SV_Position;
};
// Interpolator blocks: SV_Position goes last so that a pixel shader can declare
// the same struct minus that member.
struct 	vf_spot
{
	float2 tc0	: TEXCOORD0;	// base
	float4 tc1	: TEXCOORD1;	// lmap, projected
	float2 tc2	: TEXCOORD2;	// att + clipper
	float4 color	: COLOR0;
	float4 hpos	: SV_Position;
};
struct 	vf_point
{
	float2 tc0	: TEXCOORD0;	// base
	float2 tc1	: TEXCOORD1;	// att1 + clipper
	float2 tc2	: TEXCOORD2;	// att2 + clipper
	float4 color	: COLOR0;
	float4 hpos	: SV_Position;
};
//////////////////////////////////////////////////////////////////////////////////////////

#define def_distort	float(0.05f)	// we get -0.5 .. 0.5 range, this is -512 .. 512 for 1024, so scale it

float3	v_hemi 		(float3 n)		{	return L_hemi_color.rgb/* *(.5f + .5f*n.y) */; 		}
float3	v_hemi_wrap	(float3 n, float w)	{	return L_hemi_color.rgb/* *(w + (1-w)*n.y) */; 		}
float3 	v_sun 		(float3 n)		{	return L_sun_color*max(0,dot(n,-L_sun_dir_w));		}
float3 	v_sun_wrap	(float3 n, float w)	{	return L_sun_color*(w+(1-w)*dot(n,-L_sun_dir_w));	}
float3	p_hemi		(float2 tc) 	{
	float4	t_lmh 	= s_hemi.Sample	(smp_linear, tc);
	return  t_lmh.a;
}

#endif // COMMON_H
