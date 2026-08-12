#include "stdafx.h"
#pragma hdrstop

#include "Blender_Editor_Selection.h"

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CBlender_Editor_Selection::CBlender_Editor_Selection()
{
	description.CLS		= B_EDITOR_SEL;
	xr_strcpy				(oT_Factor,"$null");
}

CBlender_Editor_Selection::~CBlender_Editor_Selection()
{
	
}

void	CBlender_Editor_Selection::Save	( IWriter& fs	)
{
	IBlender::Save	(fs);
	xrPWRITE_PROP	(fs,"TFactor",	xrPID_CONSTANT, oT_Factor);
}

void	CBlender_Editor_Selection::Load	( IReader& fs, u16 version	)
{
	IBlender::Load	(fs,version);
	xrPREAD_PROP	(fs,xrPID_CONSTANT,	oT_Factor);
}

void	CBlender_Editor_Selection::Compile	(CBlender_Compile& C)
{
	IBlender::Compile		(C);	
#if !defined(USE_DX10) && !defined(USE_DX11)
	if (C.bEditor)	{
		C.PassBegin		();
		{
			C.PassSET_ZB		(TRUE,FALSE);
			C.PassSET_Blend		(TRUE,D3DBLEND_SRCALPHA,D3DBLEND_INVSRCALPHA,	FALSE,0);
			C.PassSET_LightFog	(FALSE,FALSE);

			// Stage0 - Base texture
			C.StageBegin		();
			C.StageSET_Address	(D3DTADDRESS_CLAMP);
			C.StageSET_Color	(D3DTA_TFACTOR,	  D3DTOP_MODULATE,		D3DTA_DIFFUSE);
			C.StageSET_Alpha	(D3DTA_TFACTOR,	  D3DTOP_MODULATE,		D3DTA_DIFFUSE);
			C.Stage_Texture		(oT_Name	);
			C.Stage_Matrix		(oT_xform,	0);
			C.Stage_Constant	("$null"	);
			C.StageEnd			();
		}
		C.PassEnd			();
	} 
	else 
#endif	//	USE_DX10
	{
		// simple_color, not editor: the selection highlight is drawn over MESH
		// geometry (position/normal/uv), and editor.vs consumes COLOR0, which
		// that geometry does not carry. D3D9 tolerated a vertex shader reading
		// an element the declaration lacked; D3D11 refuses to build the input
		// layout at all, which took the editor down the moment anything was
		// selected. simple_color.vs needs only POSITION and takes its colour
		// from tfactor - exactly what a flat highlight wants.
		//
		// Element 5 is the PRE-TRANSFORMED variant (same convention as
		// Blender_Editor_Wire): the rubber-band selection rect is FVF::F_TL,
		// screen-space with per-vertex colour, and keeps the alpha blend.
		if (5 == C.iElement)
			C.r_Pass("simple_color_t","simple_color",FALSE,FALSE,FALSE,TRUE,D3DBLEND_SRCALPHA,D3DBLEND_INVSRCALPHA);
		else
			C.r_Pass("simple_color","simple_color",FALSE,TRUE,FALSE,TRUE,D3DBLEND_SRCALPHA,D3DBLEND_INVSRCALPHA);
		C.r_End		();
	}
}
