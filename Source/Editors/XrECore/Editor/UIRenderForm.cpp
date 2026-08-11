#include "stdafx.h"
#include "UIRenderForm.h"
#include "ui_main.h"
UIRenderForm::UIRenderForm()
{
	m_mouse_down = false;
	m_mouse_move = false;
	m_shiftstate_down = false;
}

UIRenderForm::~UIRenderForm()
{

}

void UIRenderForm::Draw()
{

	ImGui::Begin("Render");
	if (UI && UI->RT->pSurface)
	{
		int ShiftState = ssNone;

		if (ImGui::GetIO().KeyShift)ShiftState |= ssShift;
		if (ImGui::GetIO().KeyCtrl)ShiftState |= ssCtrl;
		if (ImGui::GetIO().KeyAlt)ShiftState |= ssAlt;


		if (ImGui::IsMouseDown(ImGuiMouseButton_Left))ShiftState |= ssLeft;
		if (ImGui::IsMouseDown(ImGuiMouseButton_Right))ShiftState |= ssRight;
		if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))ShiftState |= ssMiddle;
		//VERIFY(!(ShiftState & ssLeft && ShiftState & ssRight));
		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
		ImVec2 canvas_size = ImGui::GetContentRegionAvail();
		ImVec2 mouse_pos = ImGui::GetIO().MousePos;
		bool cursor_in_zone = true;
		if (mouse_pos.x < canvas_pos.x)
		{
			cursor_in_zone = false;
			mouse_pos.x = canvas_pos.x;
		}
		if (mouse_pos.y < canvas_pos.y)
		{
			cursor_in_zone = false;
			mouse_pos.y = canvas_pos.y;
		}

		if (mouse_pos.x > canvas_pos.x + canvas_size.x)
		{
			cursor_in_zone = false;
			mouse_pos.x = canvas_pos.x + canvas_size.x;
		}
		if (mouse_pos.y > canvas_pos.y + canvas_size.y)
		{
			cursor_in_zone = false;
			mouse_pos.y = canvas_pos.y + canvas_size.y;
		}

		bool curent_shiftstate_down = m_shiftstate_down;


		if (canvas_size.x < 32.0f) canvas_size.x = 32.0f;
		if (canvas_size.y < 32.0f) canvas_size.y = 32.0f;
		UI->RTSize.set(canvas_size.x, canvas_size.y);

		ImGui::SetCursorScreenPos(canvas_pos);
		draw_list->AddImage(UI->RT->pSurface, canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y));

		if(m_OnToolBar)
			m_OnToolBar(canvas_size);

		ImGui::SetCursorScreenPos(canvas_pos);
		// accept every button: the default left-only button never activates on
		// RMB/MMB, which used to leave the viewport unfocused and swallow
		// camera navigation until the user left-clicked it first
		ImGui::InvisibleButton("canvas", canvas_size,
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);

		const bool any_btn_down =
			ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
			ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
			ImGui::IsMouseDown(ImGuiMouseButton_Middle);
		const bool any_btn_released =
			ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
			ImGui::IsMouseReleased(ImGuiMouseButton_Right) ||
			ImGui::IsMouseReleased(ImGuiMouseButton_Middle);

		// gate on hover, not focus — a drag started here keeps running even
		// after the cursor leaves the viewport
		if (ImGui::IsItemHovered() || m_mouse_down)
		{

			if (any_btn_down && !m_mouse_down&& cursor_in_zone)
			{
				ImGui::SetWindowFocus();
				UI->MousePress(TShiftState(ShiftState), mouse_pos.x - canvas_pos.x, mouse_pos.y - canvas_pos.y);
				m_mouse_down = true;
			}

			else  if (any_btn_released && m_mouse_down)
			{
				if (!any_btn_down)
				{
					UI->MouseRelease(TShiftState(ShiftState), mouse_pos.x - canvas_pos.x, mouse_pos.y - canvas_pos.y);
					m_mouse_down = false;
					m_mouse_move = false;
					m_shiftstate_down = false;
				}
			}
			else if (m_mouse_down)
			{
				UI->MouseMove(TShiftState(ShiftState), mouse_pos.x - canvas_pos.x, mouse_pos.y - canvas_pos.y);
				m_mouse_move = true;
				m_shiftstate_down = m_shiftstate_down||( ShiftState & (ssShift | ssCtrl | ssAlt));
			}
		}
		else  if (m_mouse_down)
		{
			if (!any_btn_down)
			{
				UI->MouseRelease(TShiftState(ShiftState), mouse_pos.x - canvas_pos.x, mouse_pos.y - canvas_pos.y);
				m_mouse_down = false;
				m_mouse_move = false;
				m_shiftstate_down = false;
			}
		}
		m_mouse_position.set(mouse_pos.x - canvas_pos.x, mouse_pos.y - canvas_pos.y);

		// asset dropped from the content browser — refresh the pick ray first so
		// the handler can raycast against whatever is under the cursor
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("XRAY_ASSET"))
			{
				if (!m_OnDropAsset.empty() && pl->Data)
				{
					EDevice->m_Camera.MouseRayFromPoint(UI->m_CurrentRStart, UI->m_CurrentRDir, m_mouse_position);
					m_OnDropAsset((LPCSTR)pl->Data);
				}
			}
			ImGui::EndDragDropTarget();
		}

		// wheel over the viewport: dolly, or speed tune while flying
		const float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.f && (ImGui::IsItemHovered() || m_mouse_down))
			UI->MouseWheel(TShiftState(ShiftState), wheel);

		// no context menu when the RMB press was camera navigation
		if (!m_OnContextMenu.empty()&& !curent_shiftstate_down && !EDevice->m_Camera.WasNavInput())
		{
			if (ImGui::BeginPopupContextItem("Menu"))
			{
				m_OnContextMenu();
				ImGui::EndPopup();
			}
		}


	}
	ImGui::End();
}
