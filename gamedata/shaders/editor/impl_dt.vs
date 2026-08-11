#include "common.h"

struct vf
{
	float2 tc0	: TEXCOORD0;
	float2 tc1	: TEXCOORD1;
	float2 tc2	: TEXCOORD2;
	float4 hpos	: SV_Position;
};
struct vv
{
	float4 P	: POSITION;
	float2 tc	: TEXCOORD0;
	float3 N	: NORMAL;
};
vf main (vv v)
{
	vf 		o;
	o.hpos 		= mul			(m_WVP, float4(v.P.xyz,1));			// xform, input in world coords
	o.tc0		= v.tc;
	o.tc1		= o.tc0;						// copy tc
	o.tc2		= o.tc0*dt_params.xy;					// dt tc

	return o;
}
