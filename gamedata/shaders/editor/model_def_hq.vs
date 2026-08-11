#include "common.h"
#include "skin.h"

// #define SKIN_2

// The FOG interpolator is gone: SM4 has no FOG semantic and no fixed-function
// fog stage, and the editor never binds fog_plane anyway.
struct vf
{
	float2 tc0	: TEXCOORD0;		// base
	float4 c	: COLOR0;		// ffp lighting, (1,1,1,1) when no lights
	float4 hpos	: SV_Position;
};

vf 	_main (v_model v)
{
	vf 		o;
	o.hpos 		= mul			(m_WVP,v.pos);		// xform, input in world coords
	o.tc0		= v.tc.xy;					// copy tc
	// the editor draws in world space (world = identity), so the normal is
	// already a world-space direction
	o.c			= float4		(calc_ffp_lighting(normalize(v.norm)), 1);
	return o;
}

/////////////////////////////////////////////////////////////////////////
#define SKIN_VF vf
#include "skin_main.h"
