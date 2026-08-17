#include "stdafx.h"
#pragma hdrstop

#include "UI_Camera.h"
#include "ui_main.h"
#include "ui_toolscustom.h"

CUI_Camera::CUI_Camera()
{
	m_Style = csPlaneMove;

	m_Znear = 0.2f;
	m_Zfar = 1500.f;
    m_HPB.set(0,0,0);
    m_Position.set(0,0,0);
    m_Target.set(0,0,0);
    m_CamMat.identity();

	m_FlySpeed = 5.f;
    m_FlyAltitude = 1.8f;

    m_bMoving=false;
    m_UENav=false;
    m_NavInput=false;
    m_MoveKeys=0;
    m_OrbitDist=10.f;
}

CUI_Camera::~CUI_Camera()
{
}

void CUI_Camera::SetStyle(ECameraStyle new_style)
{
  	if (new_style==cs3DArcBall){
	    Fvector dir;
        dir.sub			(m_Target,m_Position);
        // parse heading
   	    Fvector DYaw; DYaw.set(dir.x,0.f,dir.z); DYaw.normalize_safe();
        if (DYaw.x<0)	m_HPB.x = acosf(DYaw.z);
   	    else			m_HPB.x = 2*PI-acosf(DYaw.z);
        // parse pitch
   	    dir.normalize_safe	();
        m_HPB.y			= asinf(dir.y);

        BuildCamera		();
    }
	m_Style				= new_style;
    UI->RedrawScene();
}

void CUI_Camera::Reset()
{
	m_HPB.set(0,0,0);
    m_Position.set(0,3,-10);
	SetStyle(m_Style);
    BuildCamera();
}

const Fvector& CUI_Camera::GetPosition() const
{
    if (UI->IsPlayInEditor())
    {
        return Device->vCameraPosition;
    }
    return m_Position;
}

const Fvector& CUI_Camera::GetRight() const
{
	if (UI->IsPlayInEditor())
	{
		return Device->vCameraRight;
	}
   return m_CamMat.i;
}

const Fvector& CUI_Camera::GetNormal() const
{
	if (UI->IsPlayInEditor())
	{
		return Device->vCameraTop;
	}
    return m_CamMat.j;
}

const Fvector& CUI_Camera::GetDirection() const
{
	if (UI->IsPlayInEditor())
	{
		return Device->vCameraDirection;
	}
    return m_CamMat.k;
}

void CUI_Camera::Set(float h, float p, float b, float x, float y, float z)
{
	m_HPB.set(h,p,b);
    m_Position.set(x,y,z);
    BuildCamera();
}

void CUI_Camera::Set(const Fvector& hpb, const Fvector& pos)
{
	m_HPB.set(hpb);
    m_Position.set(pos);
    BuildCamera();
}

void CUI_Camera::Set(const Fvector& hpb, const Fvector& pos, const Fvector& target)
{
	m_HPB.set(hpb);
	m_Position.set(pos);
	m_Target.set(target);
	BuildCamera();
}

void CUI_Camera::BuildCamera()
{
	if (m_HPB.x>PI_MUL_2)  m_HPB.x-=PI_MUL_2;	if (m_HPB.x<-PI_MUL_2) m_HPB.x+=PI_MUL_2;
	if (m_HPB.y>PI_MUL_2)  m_HPB.y-=PI_MUL_2;	if (m_HPB.y<-PI_MUL_2) m_HPB.y+=PI_MUL_2;
	if (m_HPB.z>PI_MUL_2)  m_HPB.z-=PI_MUL_2;	if (m_HPB.z<-PI_MUL_2) m_HPB.z+=PI_MUL_2;

    if (m_Style==cs3DArcBall){
	    Fvector D;
    	D.setHP			(m_HPB.x,m_HPB.y);
		float dist = m_Position.distance_to(m_Target);
	    m_Position.mul	(D,-dist);
    	m_Position.add	(m_Target);
    }

	m_CamMat.setHPB(m_HPB.x,m_HPB.y,m_HPB.z);
    m_CamMat.translate_over(m_Position);
    UI->OutCameraPos();
    
	EDevice->vCameraPosition.set	(m_CamMat.c);
	EDevice->vCameraDirection.set	(m_CamMat.k);
	EDevice->vCameraTop.set		(m_CamMat.j);
    EDevice->vCameraRight.set		(m_CamMat.i);
}

void CUI_Camera::SetDepth(float _far, bool bForcedUpdate)
{
    if (m_Zfar!=_far)	{m_Zfar=_far; UI->Resize(bForcedUpdate);}
}

void CUI_Camera::SetViewport(float _near, float _far, float _fov)
{
    if (m_Znear!=_near)		{m_Znear=_near; UI->Resize();}
    if (m_Zfar!=_far)		{m_Zfar=_far; UI->Resize();}
    if (EDevice->fFOV!=_fov)	{EDevice->fFOV=_fov; UI->Resize();}
}

void CUI_Camera::SetSensitivity(float sm, float sr)
{
    m_SM = 0.2f*(sm*sm);
    m_SR = 0.02f*(sr*sr);
}

static const Fvector down_dir={0.f,-1.f,0.f};

void CUI_Camera::Update(float dt)
{
	if (m_bMoving && m_UENav){
		// UE-style fly: integrate held WASD/QE along view axes (QE = world up/down)
		if (m_MoveKeys && (m_Shift&ssRight)){
			Fvector mv = {0.f,0.f,0.f};
			if (m_MoveKeys&nmForward)	mv.add(m_CamMat.k);
			if (m_MoveKeys&nmBack)		mv.sub(m_CamMat.k);
			if (m_MoveKeys&nmRight)		mv.add(m_CamMat.i);
			if (m_MoveKeys&nmLeft)		mv.sub(m_CamMat.i);
			if (m_MoveKeys&nmUp)		mv.y += 1.f;
			if (m_MoveKeys&nmDown)		mv.y -= 1.f;
			if (mv.square_magnitude()>EPS_S){
				mv.normalize();
				mv.mul(m_FlySpeed*dt);
				m_Position.add(mv);
				BuildCamera();
				UI->RedrawScene();
			}
		}
		return;
	}
	if (m_bMoving){
    	BOOL bLeftDn = m_Shift&ssLeft;
    	BOOL bRightDn = m_Shift&ssRight;
		if ((m_Style==csFreeFly)&&(bLeftDn||bRightDn)&&!(bLeftDn&&bRightDn)){
			Fvector vmove;
	        vmove.set( m_CamMat.k );
			vmove.mul( m_FlySpeed*dt );
    		if (bLeftDn) 		m_Position.add( vmove );
    		else if (bRightDn) 	m_Position.sub( vmove );

            if (m_Shift&ssCtrl){
                float dist = UI->ZFar();
            	if (Tools->RayPick(m_Position,down_dir,dist))//UI->R PickGround(pos,m_Position,dir,-1))
                	m_Position.y = m_Position.y+down_dir.y*dist+m_FlyAltitude;
                else
                	m_Position.y = m_FlyAltitude;
                	
            }

        	UI->RedrawScene();
	    }
        BuildCamera();
    }
}

void CUI_Camera::Pan(float dx, float dz)
{
    Fvector vmove;
    vmove.set( m_CamMat.k );  vmove.y = 0;
    vmove.normalize_safe();
    vmove.mul( dz*-m_SM );
    m_Position.add( vmove );

    vmove.set( m_CamMat.i );  vmove.y = 0;
    vmove.normalize_safe();
    vmove.mul( dx*m_SM );
    m_Position.add( vmove );

    BuildCamera();
}

void CUI_Camera::Scale(float dy)
{
    Fvector vmove;
    vmove.set( 0.f, dy, 0.f );
    vmove.y *= -m_SM;
    m_Position.add( vmove );

    BuildCamera();
}

void CUI_Camera::Rotate(float dx, float dy)
{
	m_HPB.x-=m_SR*dx;
	m_HPB.y-=m_SR*dy*EDevice->fASPECT;

    BuildCamera();
}

bool CUI_Camera::MoveStart(TShiftState Shift)
{
	// UE-style claim: RMB (look/fly), MMB (pan) or Alt+LMB (orbit), all without Shift
	const bool ue_claim = !(Shift&ssShift) &&
		((Shift&ssRight) || (Shift&ssMiddle) || ((Shift&ssAlt)&&(Shift&ssLeft)));

	if ((Shift&ssShift) || ue_claim){
    	if (!m_bMoving){
		    ShowCursor	(FALSE);
    	    UI->IR_GetMousePosScreen(m_StartPos);
			m_bMoving	= true;
			m_UENav		= ue_claim;
			m_MoveKeys	= 0;
			m_NavInput	= false;
			if (m_UENav){
				// orbit pivot: reuse the focus target when sane, else project one ahead
				float d = m_Position.distance_to(m_Target);
				if (d>0.5f && d<500.f)	m_OrbitDist = d;
				else					{ m_OrbitDist = 10.f; m_Target.mad(m_Position, m_CamMat.k, m_OrbitDist); }
			}
        }
		m_Shift 	= Shift;
        return true;
    }
	m_Shift = Shift;
    return false;
}

bool CUI_Camera::MoveEnd(TShiftState Shift)
{
	m_Shift = Shift;
	const bool nav_end		= m_UENav && !(Shift&(ssLeft|ssRight|ssMiddle));
	const bool legacy_end	= !m_UENav && ((!Shift&ssLeft)||(!Shift&ssShift));
	if (nav_end || legacy_end){
	    SetCursorPos(m_StartPos.x, m_StartPos.y);
    	ShowCursor	(TRUE);
		m_bMoving	= false;
		m_UENav		= false;
		m_MoveKeys	= 0;
        return true;
    }
    return false;
}

bool CUI_Camera::Process(TShiftState Shift, int dx, int dy)
{
    if (m_bMoving){
        m_Shift = Shift;
// camera move
        if( dx || dy ){
        	SetCursorPos(m_StartPos.x,m_StartPos.y);
            if (m_UENav){
                m_NavInput = true;
                const bool L = !!(m_Shift&ssLeft), R = !!(m_Shift&ssRight);
                const bool M = !!(m_Shift&ssMiddle), A = !!(m_Shift&ssAlt);
                if (A && L && !R){
                    // orbit around the pivot at fixed distance
                    m_HPB.x -= m_SR*dx;
                    m_HPB.y -= m_SR*dy*EDevice->fASPECT;
                    Fvector D; D.setHP(m_HPB.x,m_HPB.y);
                    m_Position.mul(D,-m_OrbitDist);
                    m_Position.add(m_Target);
                    BuildCamera();
                }else if (A && R && !L){
                    // dolly: drag up = in, drag down = out
                    float d = dy*m_SM;
                    m_Position.mad(m_Position, m_CamMat.k, -d);
                    m_OrbitDist += d;
                    clamp(m_OrbitDist, 0.5f, 5000.f);
                    BuildCamera();
                }else if ((L && R) || M){
                    // pan: grab-the-world — drag right moves camera left
                    Fvector mv;
                    mv.mul(m_CamMat.i, -dx*m_SM);
                    m_Position.add(mv);
                    m_Position.y += dy*m_SM;
                    BuildCamera();
                }else if (R){
                    Rotate(dx, dy);
                }
            }else{
            switch (m_Style){
            case csPlaneMove:
                if ((m_Shift & ssLeft) && (m_Shift & ssRight))
                {
                    Rotate(dx, dy);
                }
                else if (m_Shift & ssLeft)
                {
                    Pan(dx, dy);
                }
                else if (m_Shift &ssRight)
                {
                    Scale(dy);
                }
            break;
            case csFreeFly:
                if ((m_Shift&ssLeft)||(m_Shift&ssRight)) Rotate (dx,dy);
//                if (Shift&ssLeft)) Rotate (d.x,d.y);
//                else if (Shift&ssRight)) Scale(d.y);
            break;
            case cs3DArcBall:
            	ArcBall(m_Shift,dx,dy);
            break;
            }
            }
		    UI->RedrawScene();
        }
        return true;
	}
    return false;
}

bool CUI_Camera::KeyDown(WORD Key, TShiftState Shift)
{
    if (m_bMoving){
        // UE-style fly: WASD + QE while RMB is held; consume so tool hotkeys stay quiet
        if (m_UENav && (m_Shift&ssRight)){
            switch (Key){
            case 'W': m_MoveKeys |= nmForward;	break;
            case 'S': m_MoveKeys |= nmBack;		break;
            case 'A': m_MoveKeys |= nmLeft;		break;
            case 'D': m_MoveKeys |= nmRight;	break;
            case 'E': m_MoveKeys |= nmUp;		break;
            case 'Q': m_MoveKeys |= nmDown;		break;
            case VK_CONTROL: m_Shift = ssCtrl | m_Shift; return true;
            default: return false;
            }
            m_NavInput = true;
            return true;
        }
    	switch (Key){
        case VK_CONTROL:  m_Shift = ssCtrl | m_Shift; break;
        default: return false;
        }
	    return true;
    }
	return false;
}

bool CUI_Camera::KeyUp(WORD Key, TShiftState Shift)
{
    if (m_bMoving){
        if (m_UENav){
            switch (Key){
            case 'W': m_MoveKeys &= ~nmForward;	return true;
            case 'S': m_MoveKeys &= ~nmBack;	return true;
            case 'A': m_MoveKeys &= ~nmLeft;	return true;
            case 'D': m_MoveKeys &= ~nmRight;	return true;
            case 'E': m_MoveKeys &= ~nmUp;		return true;
            case 'Q': m_MoveKeys &= ~nmDown;	return true;
            case VK_CONTROL: m_Shift = ~ssCtrl & m_Shift; return true;
            default: return false;
            }
        }
    	switch (Key){
        case VK_SHIFT:  m_Shift = ~ssShift & m_Shift; MoveEnd(m_Shift); break;
        case VK_CONTROL: m_Shift = ~ssCtrl & m_Shift; break;
        default: return false;
        }
	    return true;
    }
	return false;
}

void CUI_Camera::Wheel(TShiftState Shift, float steps)
{
	if (m_bMoving && m_UENav && (m_Shift&ssRight)){
		// UE: wheel while flying tunes the camera speed exponentially
		m_FlySpeed *= powf(1.2f, steps);
		clamp(m_FlySpeed, 0.1f, 200.f);
		string64 tmp;
		xr_sprintf(tmp, "Camera speed: %.1f", m_FlySpeed);
		UI->SetStatus(tmp, false);
		return;
	}
	// UE: plain wheel dollies along the view direction, scaled by fly speed
	Fvector mv;
	mv.mul(m_CamMat.k, steps * _max(0.5f, m_FlySpeed*0.35f));
	m_Position.add(mv);
	BuildCamera();
	UI->RedrawScene();
}

void CUI_Camera::MouseRayFromPoint( Fvector& start, Fvector& direction, const Ivector2& point )
{
	int halfwidth  = UI->GetRenderWidth()*0.5f;
	int halfheight = UI->GetRenderHeight()*0.5f;

    if (!halfwidth||!halfheight) return;

    Ivector2 point2;
    point2.set(point.x-halfwidth, halfheight-point.y);

	start.set( m_Position );

	float size_y = m_Znear * tan( deg2rad(EDevice->fFOV) * 0.5f );
	float size_x = size_y / EDevice->fASPECT;

	float r_pt = float(point2.x) * size_x / (float) halfwidth;
	float u_pt = float(point2.y) * size_y / (float) halfheight;

	direction.mul( m_CamMat.k, m_Znear );
	direction.mad( direction, m_CamMat.j, u_pt );
	direction.mad( direction, m_CamMat.i, r_pt );
	direction.normalize();
}

void CUI_Camera::ZoomExtents(const Fbox& bb)
{
	Fvector C,D;
    float R,H1,H2;
    bb.getsphere(C,R);
	D.mul(m_CamMat.k,-1);
    H1 = R/sinf(deg2rad(EDevice->fFOV)*0.5f);
    H2 = R/sinf(deg2rad(EDevice->fFOV)*0.5f/EDevice->fASPECT);
    m_Position.mad(C,D,_max(H1,H2));
	m_Target.set(C);

	BuildCamera();
/*
	eye_k - �������� ����������, eye_k=eye_width/2
	camera.alfa:=0;
     camera.beta:=-30*pi/180;
     camera.gama:=0;
     s:=(maxx-minx)*eye_k/eye_width*0.5*0.5;
     camera.posx:=(maxx+minx)/2;
     camera.posy:=maxy+s*tan(30*pi/180);
     camera.posz:=minz-s;
*/
}

void CUI_Camera::ArcBall(TShiftState Shift, float dx, float dy)
{
	float dist = m_Position.distance_to(m_Target);
	if (Shift&ssAlt){
		if (Shift&ssLeft){
            Fvector vmove;
            vmove.set( m_CamMat.k );  vmove.y = 0;
            vmove.normalize_safe();
            vmove.mul( dy*-m_SM );
            m_Target.add( vmove );

            vmove.set( m_CamMat.i );  vmove.y = 0;
            vmove.normalize_safe();
            vmove.mul( dx*m_SM );
            m_Target.add( vmove );
        }else if(Shift&ssRight){
            Fvector vmove;
            vmove.set( 0.f, dy, 0.f );
            vmove.y *= -m_SM;
            m_Target.add( vmove );
        }
    }else{
    	if (Shift&ssRight){
        	dist -= dx*m_SM;
	    }else if (Shift&ssLeft){
    	    m_HPB.x-=m_SR*dx;
        	m_HPB.y-=m_SR*dy*EDevice->fASPECT;
	    }
    }

    Fvector D;
    D.setHP			(m_HPB.x,m_HPB.y);

    m_Position.mul	(D,-dist);
    m_Position.add	(m_Target);

	BuildCamera		();
}


