#include "stdafx.h"
#include "dxFontRender.h"

#include "../../xrEngine/GameFont.h"

dxFontRender::dxFontRender()
{

}

dxFontRender::~dxFontRender()
{
	pShader.destroy();
	pGeom.destroy();
}

void dxFontRender::Initialize(LPCSTR cShader, LPCSTR cTexture)
{
	pShader.create(cShader, cTexture);
	pGeom.create(FVF::F_TL, RCache.Vertex.Buffer(), RCache.QuadIB);
}
extern ENGINE_API BOOL g_bRendering;
extern ENGINE_API Fvector2		g_current_font_scale;

// -trace breadcrumbs, same switch ui_main.cpp uses. D3D11 bring-up aid.
namespace
{
	const bool	s_font_trace	= !!strstr(GetCommandLineA(), "-trace");
	u32			s_font_left		= 2;
}
#define FONT_TRACE(...)	do { if (s_font_trace && s_font_left) Msg(__VA_ARGS__); } while(0)
#define FONT_BC(x)		Breadcrumb("font: " x)

void dxFontRender::OnRender(CGameFont &owner)
{
	VERIFY				(g_bRendering);
	FONT_BC("enter");
	FONT_TRACE("~ font: enter, shader=%d geom=%d strings=%d", !!pShader, !!pGeom, int(owner.strings.size()));
	if (pShader)
	{
		FONT_TRACE("~ font: passes=%d", pShader->E[0] ? int(pShader->E[0]->passes.size()) : -1);
		if (s_font_trace && s_font_left && pShader->E[0] && pShader->E[0]->passes.size())
		{
			SPass& P = *(pShader->E[0]->passes[0]);
			Msg("~ font: pass state=%d vs=%d ps=%d ct=%d T=%d M=%d",
				!!P.state._get(), !!P.vs._get(), !!P.ps._get(),
				!!P.constants._get(), !!P.T._get(), !!P.M._get());
		}
		// element 5, not 0: the font's geometry is FVF::F_TL - already in pixel
		// coordinates - and that is the variant Blender_Screen_SET compiles
		// without a transform. Element 0 transforms, which is what the editor's
		// world-space sprites through the same blender need.
		if (pShader->E[5])	RCache.set_Element	(pShader->E[5]);
		else				RCache.set_Shader	(pShader);
	}
	FONT_BC("shader set");
	FONT_TRACE("~ font: shader set");
	FONT_TRACE("~ font: stride=%d", pGeom ? int(pGeom.stride()) : -1);

	if (!(owner.uFlags&CGameFont::fsValid)){
		CTexture* T		= RCache.get_ActiveTexture(0);
		// A font with no texture bound at stage 0 has nothing to measure itself
		// against and nothing to draw. Under D3D11 this happens when the font's
		// shader failed to compile and fell back to a stub, which used to be an
		// instant null dereference instead of a diagnosable message.
		if (!T)
		{
			static bool reported = false;
			if (!reported)
			{
				reported = true;
				Msg	("! font: no texture bound at stage 0, text will not be drawn");
			}
			return;
		}
		owner.vTS.set			((int)T->get_Width(),(int)T->get_Height());
		owner.fTCHeight		= owner.fHeight/float(owner.vTS.y);
		owner.uFlags			|= CGameFont::fsValid;
	}

	for (u32 i=0; i<owner.strings.size(); ){
		// calculate first-fit
		int		count	=	1;

		int length = owner.smart_strlen( owner.strings[ i ].string );

		while	((i+count)<owner.strings.size()) {
			int L = owner.smart_strlen( owner.strings[ i + count ].string );

			if ((L+length)<MAX_MB_CHARS){
				count	++;
				length	+=	L;
			}
			else		break;
		}

		// lock AGP memory
		u32	vOffset;
		FONT_BC("before lock");
		FONT_TRACE("~ font: lock %d verts, stride %d", length*4, int(pGeom.stride()));
		FVF::TL* v		= (FVF::TL*)RCache.Vertex.Lock	(length*4,pGeom.stride(),vOffset);
		FONT_BC("after lock");
		FONT_TRACE("~ font: locked ptr=%s offset=%d", v ? "ok" : "NULL", int(vOffset));
		FVF::TL* start	= v;

		// fill vertices
		u32 last		= i+count;
		for (; i<last; i++) {
			CGameFont::String		&PS	= owner.strings[i];
			wide_char wsStr[ MAX_MB_CHARS ];

			int	len	= owner.IsMultibyte() ? 
				mbhMulti2Wide( wsStr , NULL , MAX_MB_CHARS , PS.string ) :
			xr_strlen( PS.string );

			if (len) {
				float	X	= float(iFloor(PS.x));
				float	Y	= float(iFloor(PS.y));
				float	S	= PS.height*g_current_font_scale.y;
				float	Y2	= Y+S;
				float fSize = 0;

				if ( PS.align )
					fSize = owner.IsMultibyte() ? owner.SizeOf_( wsStr ) : owner.SizeOf_( PS.string );

				switch ( PS.align )
				{
				case CGameFont::alCenter:	
					X	-= ( iFloor( fSize * 0.5f ) ) * g_current_font_scale.x;	
					break;
				case CGameFont::alRight:	
					X	-=	iFloor( fSize );
					break;
				}

				u32	clr,clr2;
				clr2 = clr	= PS.c;
				if (owner.uFlags&CGameFont::fsGradient){
					u32	_R	= color_get_R	(clr)/2;
					u32	_G	= color_get_G	(clr)/2;
					u32	_B	= color_get_B	(clr)/2;
					u32	_A	= color_get_A	(clr);
					clr2	= color_rgba	(_R,_G,_B,_A);
				}

#if defined(USE_DX10) || defined(USE_DX11)		//	Vertex shader will cancel a DX9 correction, so make fake offset
				X			-= 0.5f;
				Y			-= 0.5f;
				Y2			-= 0.5f;
#endif	//	USE_DX10

				float	tu,tv;
				for (int j=0; j<len; j++)
				{
					Fvector l;

					l = owner.IsMultibyte() ? owner.GetCharTC( wsStr[ 1 + j ] ) : owner.GetCharTC( ( u16 ) ( u8 ) PS.string[j] );

					float scw		= l.z * g_current_font_scale.x;

					float fTCWidth	= l.z/owner.vTS.x;

					if (!fis_zero(l.z))
					{
//						tu			= ( l.x / owner.vTS.x ) + ( 0.5f / owner.vTS.x );
//						tv			= ( l.y / owner.vTS.y ) + ( 0.5f / owner.vTS.y );
						tu			= ( l.x / owner.vTS.x );
						tv			= ( l.y / owner.vTS.y );
#ifndef	USE_DX10
						//	Make half pixel offset for 1 to 1 mapping
						tu			+=( 0.5f / owner.vTS.x );
						tv			+=( 0.5f / owner.vTS.y );
#endif	//	USE_DX10

						v->set( X , Y2 , clr2 , tu , tv + owner.fTCHeight );						v++;
						v->set( X ,	Y , clr , tu , tv );									v++;
						v->set( X + scw , Y2 , clr2 , tu + fTCWidth , tv + owner.fTCHeight );		v++;
						v->set( X + scw , Y , clr , tu + fTCWidth , tv );					v++;
					}
					X += scw * owner.vInterval.x;
					if ( owner.IsMultibyte() ) {
						X -= 2;
						if ( IsNeedSpaceCharacter( wsStr[ 1 + j ] ) )
							X += owner.fXStep;
					}
				}
			}
		}

		// Unlock and draw
		u32 vCount = (u32)(v-start);
		FONT_BC("filled");
		FONT_TRACE("~ font: filled %d of %d verts", int(vCount), length*4);
		RCache.Vertex.Unlock		(vCount,pGeom.stride());
		FONT_BC("unlocked");
		if (vCount){
			RCache.set_Geometry		(pGeom);
			FONT_BC("before Render");
			FONT_TRACE("~ font: before Render");
			RCache.Render			(D3DPT_TRIANGLELIST,vOffset,0,vCount,0,vCount/2);
			FONT_BC("after Render");
			FONT_TRACE("~ font: after Render");
		}
	}
	if (s_font_left)	--s_font_left;
}