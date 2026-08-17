#include "stdafx.h"
#pragma hdrstop

#include "UI_ViewportNavigation.h"
#include "UI_MainCommand.h"
#include "UI_ToolsCustom.h"
#include "ui_main.h"

namespace
{
ViewportNavigation::Target g_CurrentTarget = ViewportNavigation::Target::Perspective;
Fvector g_PerspectiveHPB;
Fvector g_PerspectivePosition;
Fvector g_PerspectiveTarget;
bool g_HasPerspectivePose = false;

ImU32 ThemeColor(XFinedTheme::ColorToken token, u8 alpha = 0xff)
{
	const u32 rgb = XFinedTheme::Rgb(token);
	return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, alpha);
}

ImVec4 ThemeColorVec(XFinedTheme::ColorToken token, float alpha = 1.f)
{
	const u32 rgb = XFinedTheme::Rgb(token);
	return ImVec4(
		float((rgb >> 16) & 0xff) / 255.f,
		float((rgb >> 8) & 0xff) / 255.f,
		float(rgb & 0xff) / 255.f,
		alpha);
}

bool MatchesOrientation(ViewportNavigation::Target target)
{
	if (!EDevice || target == ViewportNavigation::Target::Perspective)
		return true;

	Fvector expected;
	switch (target)
	{
	case ViewportNavigation::Target::Front:	expected.set(0.f, 0.f, 0.f); break;
	case ViewportNavigation::Target::Back:	expected.set(M_PI, 0.f, 0.f); break;
	case ViewportNavigation::Target::Left:	expected.set(-PI_DIV_2, 0.f, 0.f); break;
	case ViewportNavigation::Target::Right:	expected.set(PI_DIV_2, 0.f, 0.f); break;
	case ViewportNavigation::Target::Top:	expected.set(0.f, -PI_DIV_2, 0.f); break;
	case ViewportNavigation::Target::Bottom:	expected.set(0.f, PI_DIV_2, 0.f); break;
	default:							return false;
	}

	const Fvector& actual = EDevice->m_Camera.GetHPB();
	const float tolerance = deg2rad(.1f);
	return angle_difference(actual.x, expected.x) <= tolerance &&
		angle_difference(actual.y, expected.y) <= tolerance &&
		angle_difference(actual.z, expected.z) <= tolerance;
}

void RefreshCurrentTarget()
{
	if (!MatchesOrientation(g_CurrentTarget))
		g_CurrentTarget = ViewportNavigation::Target::Perspective;
}

void DrawAxisWidget(const ImVec2& origin, float radius)
{
	if (!EDevice)
		return;

	struct Axis
	{
		Fvector direction;
		LPCSTR label;
		XFinedTheme::ColorToken color;
	};

	const Axis axes[] = {
		{{1.f, 0.f, 0.f}, "X", XFinedTheme::ColorToken::Error},
		{{0.f, 1.f, 0.f}, "Y", XFinedTheme::ColorToken::Success},
		{{0.f, 0.f, 1.f}, "Z", XFinedTheme::ColorToken::AccentActive},
	};
	const Fvector& camera_right = EDevice->m_Camera.GetRight();
	const Fvector& camera_up = EDevice->m_Camera.GetNormal();
	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	draw_list->AddCircleFilled(origin, 3.f, ThemeColor(XFinedTheme::ColorToken::Text));

	for (const Axis& axis : axes)
	{
		// Project each world axis onto the camera's screen basis.
		const ImVec2 projected(
			axis.direction.dotproduct(camera_right),
			-axis.direction.dotproduct(camera_up));
		const ImVec2 positive(origin.x + projected.x * radius, origin.y + projected.y * radius);
		const ImVec2 negative(origin.x - projected.x * radius * .58f, origin.y - projected.y * radius * .58f);
		const ImU32 color = ThemeColor(axis.color);

		draw_list->AddLine(negative, origin, ThemeColor(XFinedTheme::ColorToken::Muted, 0x78), 1.f);
		draw_list->AddLine(origin, positive, color, 2.f);
		draw_list->AddCircleFilled(positive, 7.f, color);

		const ImVec2 label_size = ImGui::CalcTextSize(axis.label);
		draw_list->AddText(
			ImVec2(positive.x - label_size.x * .5f, positive.y - label_size.y * .5f),
			ThemeColor(XFinedTheme::ColorToken::Background), axis.label);
	}
}

bool TargetButton(ViewportNavigation::Target target, float width)
{
	const bool selected = ViewportNavigation::CurrentTarget() == target;
	if (selected)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ThemeColorVec(XFinedTheme::ColorToken::Accent));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColorVec(XFinedTheme::ColorToken::AccentHovered));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColorVec(XFinedTheme::ColorToken::AccentActive));
	}

	const bool clicked = ImGui::Button(ViewportNavigation::TargetName(target), ImVec2(width, 0.f));
	if (selected)
		ImGui::PopStyleColor(3);
	if (ImGui::IsItemHovered())
	{
		if (target == ViewportNavigation::Target::Perspective)
			ImGui::SetTooltip("Return to the previous free perspective view");
		else
			ImGui::SetTooltip("Align the camera to the %s axis view", ViewportNavigation::TargetName(target));
	}
	if (clicked)
		ViewportNavigation::SetTarget(target);
	return clicked;
}

bool FrameButton(LPCSTR label, LPCSTR tooltip, bool selection, float width)
{
	if (!ImGui::Button(label, ImVec2(width, 0.f)))
	{
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);
		return false;
	}

	if (selection)
		return ViewportNavigation::FrameSelection();
	return ViewportNavigation::FrameAll();
}
}

bool ViewportNavigation::SetTarget(Target target)
{
	if (!EDevice || !UI || UI->IsPlayInEditor() || u32(target) >= u32(Target::Count))
		return false;

	RefreshCurrentTarget();
	const Target previous_target = g_CurrentTarget;

	if (target != Target::Perspective && previous_target == Target::Perspective)
	{
		g_PerspectiveHPB = EDevice->m_Camera.GetHPB();
		g_PerspectivePosition = EDevice->m_Camera.GetPosition();
		g_PerspectiveTarget = EDevice->m_Camera.GetTarget();
		g_HasPerspectivePose = true;
	}

	switch (target)
	{
	case Target::Perspective:
		if (previous_target != Target::Perspective && g_HasPerspectivePose)
			EDevice->m_Camera.Set(g_PerspectiveHPB, g_PerspectivePosition, g_PerspectiveTarget);
		break;
	case Target::Front:		EDevice->m_Camera.ViewFront(); break;
	case Target::Back:		EDevice->m_Camera.ViewBack(); break;
	case Target::Left:		EDevice->m_Camera.ViewLeft(); break;
	case Target::Right:		EDevice->m_Camera.ViewRight(); break;
	case Target::Top:			EDevice->m_Camera.ViewTop(); break;
	case Target::Bottom:		EDevice->m_Camera.ViewBottom(); break;
	default:					return false;
	}

	g_CurrentTarget = target;
	UI->RedrawScene();
	return true;
}

ViewportNavigation::Target ViewportNavigation::CurrentTarget()
{
	RefreshCurrentTarget();
	return g_CurrentTarget;
}

void ViewportNavigation::ResetState()
{
	g_CurrentTarget = Target::Perspective;
	g_HasPerspectivePose = false;
}

bool ViewportNavigation::TryParseTarget(LPCSTR value, Target& target)
{
	if (!value || !value[0])
		return false;
	if (0 == _stricmp(value, "reset"))
	{
		target = Target::Perspective;
		return true;
	}

	for (u32 i = 0; i < u32(Target::Count); ++i)
	{
		const Target candidate = Target(i);
		if (0 == _stricmp(value, TargetKey(candidate)) || 0 == _stricmp(value, TargetName(candidate)))
		{
			target = candidate;
			return true;
		}
	}
	return false;
}

LPCSTR ViewportNavigation::TargetKey(Target target)
{
	switch (target)
	{
	case Target::Perspective:	return "perspective";
	case Target::Front:		return "front";
	case Target::Back:		return "back";
	case Target::Left:		return "left";
	case Target::Right:		return "right";
	case Target::Top:			return "top";
	case Target::Bottom:		return "bottom";
	default:					return "perspective";
	}
}

LPCSTR ViewportNavigation::TargetName(Target target)
{
	switch (target)
	{
	case Target::Perspective:	return "Perspective";
	case Target::Front:		return "Front";
	case Target::Back:		return "Back";
	case Target::Left:		return "Left";
	case Target::Right:		return "Right";
	case Target::Top:			return "Top";
	case Target::Bottom:		return "Bottom";
	default:					return "Perspective";
	}
}

bool ViewportNavigation::FrameAll()
{
	if (!UI || !Tools || UI->IsPlayInEditor())
		return false;
	return ExecCommand(COMMAND_ZOOM_EXTENTS, FALSE);
}

bool ViewportNavigation::FrameSelection()
{
	if (!UI || !Tools || UI->IsPlayInEditor())
		return false;
	return ExecCommand(COMMAND_ZOOM_EXTENTS, TRUE);
}

bool ViewportNavigation::Draw(const ImVec2& canvas_position, const ImVec2& canvas_size)
{
	if (!EDevice || !UI || UI->IsPlayInEditor() || canvas_size.x < 190.f || canvas_size.y < 230.f)
		return false;

	RefreshCurrentTarget();
	const float scale = _min(2.f, _max(.85f, ImGui::GetFontSize() / 13.f));
	const float margin = 8.f * scale;
	const float panel_width = _min(180.f * scale, canvas_size.x - margin * 2.f);
	const float button_height = ImGui::GetFrameHeight();
	const float axis_height = 70.f * scale;
	const float panel_height = axis_height + button_height * 5.f + ImGui::GetStyle().ItemSpacing.y * 6.f + margin;
	const float top_offset = ImGui::GetFrameHeightWithSpacing() + margin;
	if (panel_width < 170.f || panel_height + top_offset + margin > canvas_size.y)
		return false;
	const ImVec2 panel_position(
		canvas_position.x + canvas_size.x - panel_width - margin,
		canvas_position.y + top_offset);
	const ImVec2 panel_end(panel_position.x + panel_width, panel_position.y + panel_height);
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool owns_mouse = mouse.x >= panel_position.x && mouse.x <= panel_end.x &&
		mouse.y >= panel_position.y && mouse.y <= panel_end.y;

	ImGui::SetCursorScreenPos(panel_position);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(margin, margin * .75f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.f * scale);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColorVec(XFinedTheme::ColorToken::Panel, .94f));
	ImGui::PushStyleColor(ImGuiCol_Border, ThemeColorVec(XFinedTheme::ColorToken::Border, .95f));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs;
	if (ImGui::BeginChild("##ViewportNavigation", ImVec2(panel_width, panel_height), true, flags))
	{
		const ImVec2 content_start = ImGui::GetCursorScreenPos();
		DrawAxisWidget(
			ImVec2(content_start.x + ImGui::GetContentRegionAvail().x * .5f, content_start.y + axis_height * .44f),
			24.f * scale);
		ImGui::Dummy(ImVec2(0.f, axis_height));

		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float half_width = (ImGui::GetContentRegionAvail().x - spacing) * .5f;
		TargetButton(Target::Front, half_width);
		ImGui::SameLine();
		TargetButton(Target::Back, half_width);
		TargetButton(Target::Left, half_width);
		ImGui::SameLine();
		TargetButton(Target::Right, half_width);
		TargetButton(Target::Top, half_width);
		ImGui::SameLine();
		TargetButton(Target::Bottom, half_width);
		TargetButton(Target::Perspective, ImGui::GetContentRegionAvail().x);
		FrameButton("Frame All", "Frame every visible object", false, half_width);
		ImGui::SameLine();
		FrameButton("Selection", "Frame the current selection", true, half_width);
	}
	ImGui::EndChild();
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(2);
	return owns_mouse;
}
