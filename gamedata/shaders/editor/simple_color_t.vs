#include "common.h"

// Pre-transformed flat-colour lines (FVF::F_TL): DU's screen-space debug
// markers - placements, per-object axes. The position arrives in pixels, so
// this only does the viewport mapping (same one stub_notransform_t uses, so
// the lines land exactly where the font expects them), and the colour is the
// vertex's own, tinted by tfactor.
// no TEXCOORD input on purpose: every signature entry must exist in the
// vertex declaration for CreateInputLayout, and keeping the input down to
// POSITIONT+COLOR0 lets this shader serve F_TL and F_TL0uv geometry alike
// (extra declaration elements are fine, missing ones are fatal)
struct vv
{
	float4 P	: POSITIONT;
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

	v.P.xy		+= 0.5f;
	o.hpos.x	= v.P.x * screen_res.z * 2 - 1;
	o.hpos.y	= (v.P.y * screen_res.w * 2 - 1) * -1;
	o.hpos.zw	= v.P.zw;

	o.C			= tfactor * unpack_D3DCOLOR(v.C);

	return o;
}
