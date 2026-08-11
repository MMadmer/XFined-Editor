#include "common.h"
#include "skin.h"

// #define SKIN_2

struct vf
{
	float2 tc0	: TEXCOORD0;		// base
	float3 tc1	: TEXCOORD1;		// environment
	float4 hpos	: SV_Position;
};

vf 	_main (v_model v)
{
	vf 		o;

	float4 	pos 	= v.pos;
	float3  pos_w 	= mul			(m_W, pos);
	// m_W is 3x4 - only its rotation part may touch a normal
	float3 	norm_w 	= normalize 		(mul((float3x3)m_W, v.norm));

	o.hpos 		= mul			(m_WVP, pos);		// xform, input in world coords
	o.tc0		= v.tc.xy;					// copy tc
	o.tc1		= calc_reflection	(pos_w, norm_w);
	return o;
}

/////////////////////////////////////////////////////////////////////////
#define SKIN_VF vf
#include "skin_main.h"
