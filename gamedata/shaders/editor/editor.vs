#include "common.h"

// Vertex shader for the wire / selection passes (Blender_Editor_Wire,
// Blender_Editor_Selection). Geometry comes from FVF::F_L - position + D3DCOLOR.
struct vv
{
	float4 P	: POSITION;
	float4 C	: COLOR0;
};

struct vf
{
	float4 C	: COLOR0;
	float4 hpos	: SV_Position;
};

uniform float4 		tfactor;

vf main (vv v)
{
	vf 		o;

	o.hpos 		= mul	(m_WVP, v.P);			// xform, input in world coords
	// the D3D9 pass modulated DIFFUSE by TFACTOR in a fixed-function stage;
	// the vertex colour is a D3DCOLOR, hence the channel swap
	o.C 		= tfactor*unpack_D3DCOLOR(v.C);

	return o;
}
