#include "stdafx.h"
#pragma hdrstop

#include "UI_ProgressCenter.h"
#include "ui_main.h"

namespace
{
ImVec4 ThemeColor(XFinedTheme::ColorToken token, float alpha = 1.f)
{
	const u32 rgb = XFinedTheme::Rgb(token);
	return ImVec4(
		float((rgb >> 16) & 0xff) / 255.f,
		float((rgb >> 8) & 0xff) / 255.f,
		float(rgb & 0xff) / 255.f,
		alpha);
}
}

void UIProgressCenter::FormatElapsed(u64 elapsed_ms, xr_string& result)
{
	const u64 total_seconds = elapsed_ms / 1000;
	const u64 hours = total_seconds / 3600;
	const u64 minutes = (total_seconds / 60) % 60;
	const u64 seconds = total_seconds % 60;

	if (hours)
		result.sprintf("%llu:%02llu:%02llu", hours, minutes, seconds);
	else
		result.sprintf("%02llu:%02llu", minutes, seconds);
}

void UIProgressCenter::Draw(TUI& ui)
{
	SProgressTaskInfoVec tasks;
	ui.GetProgressSnapshot(tasks);
	if (tasks.empty())
		return;

	ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImVec2 padding(12.f, 12.f);
	const float panel_width = _min(420.f, viewport->WorkSize.x - padding.x * 2.f);
	ImGui::SetNextWindowPos(
		ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - padding.x,
			viewport->WorkPos.y + viewport->WorkSize.y - padding.y),
		ImGuiCond_Always, ImVec2(1.f, 1.f));
	ImGui::SetNextWindowSizeConstraints(
		ImVec2(_min(320.f, panel_width), 0.f),
		ImVec2(panel_width, viewport->WorkSize.y * .55f));
	ImGui::SetNextWindowViewport(viewport->ID);
	ImGui::SetNextWindowBgAlpha(.96f);

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_AlwaysAutoResize;
	if (!ImGui::Begin("Task Center##XFinedProgress", nullptr, flags))
	{
		ImGui::End();
		return;
	}

	ImGui::Text("Tasks (%u)", u32(tasks.size()));
	ImGui::Separator();
	for (const SProgressTaskInfo& task : tasks)
	{
		if (task.depth)
			ImGui::Indent(float(task.depth) * 12.f);

		ImGui::TextUnformatted(task.text.c_str());
		if (!task.detail.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(XFinedTheme::ColorToken::Muted));
			ImGui::TextWrapped("%s", task.detail.c_str());
			ImGui::PopStyleColor();
		}

		xr_string overlay;
		if (task.determinate)
			overlay.sprintf("%.0f%%", task.fraction * 100.f);
		else
			overlay = "Working...";
		ImGui::ProgressBar(task.fraction, ImVec2(-FLT_MIN, 0.f), overlay.c_str());

		xr_string elapsed;
		FormatElapsed(task.elapsed_ms, elapsed);
		ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(XFinedTheme::ColorToken::Muted));
		ImGui::Text("Elapsed %s", elapsed.c_str());
		ImGui::PopStyleColor();

		if (task.depth)
			ImGui::Unindent(float(task.depth) * 12.f);
		if (&task != &tasks.back())
			ImGui::Separator();
	}

	const bool cancel_requested = tasks.front().cancel_requested;
	if (cancel_requested)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(XFinedTheme::ColorToken::Warning));
		ImGui::TextUnformatted("Cancellation requested...");
		ImGui::PopStyleColor();
	}
	else if (ImGui::Button("Cancel operation"))
	{
		ui.NeedBreak();
		ELog.Msg(mtInformation, "Cancellation requested by user.");
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Cancellation is cooperative and is handled when the task yields to the editor UI");

	ImGui::End();
}
