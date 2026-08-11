#include "common.h"

//////////////////////////////////////////////////////////////////////////////////////////
// Vertex
// Fallback the resource manager substitutes when a .vs is missing - without it
// the miss path dereferences a null reader instead of reporting anything.
v2p_TL main ( v_TL I )
{
	v2p_TL O;

	O.HPos = mul( m_WVP, I.P);
	O.Tex0 = I.Tex0;
	O.Color = I.Color.bgra;	//	swizzle vertex colour

 	return O;
}
