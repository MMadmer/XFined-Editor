#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "stdafx.h"
#include "imgui_internal.h"
float ImGui::GetWindowBarHeight()
{
    ImGuiWindow* window = GImGui->CurrentWindow;
    return window->MenuBarHeight;
}
bool ImGui::OpenPopupOnItemClick2(const char* str_id, ImGuiPopupFlags popup_flags)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    int mouse_button = (popup_flags & ImGuiPopupFlags_MouseButtonMask_);
    if (IsMouseReleased(mouse_button) && IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup))
    {
        ImGuiID id = str_id ? window->GetID(str_id) : g.LastItemData.ID;    // If user hasn't passed an ID, we can use the LastItemID. Using LastItemID as a Popup ID won't conflict!
        IM_ASSERT(id != 0);                                             // You cannot pass a NULL str_id if the last item has no identifier (e.g. a Text() item)
        OpenPopupEx(id, popup_flags);
        return true;
    }
    return false;
}

bool ImGui::ImageButton(const char* str_id, ImTextureRef tex_ref, const ImVec2& image_size,
    const ImVec2& uv0, const ImVec2& uv1, int frame_padding,
    const ImVec4& bg_col, const ImVec4& tint_col)
{
    if (frame_padding >= 0)
        PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(float(frame_padding), float(frame_padding)));
    const bool pressed = ImageButton(str_id, tex_ref, image_size, uv0, uv1, bg_col, tint_col);
    if (frame_padding >= 0)
        PopStyleVar();
    return pressed;
}

void ImGui::PushMixedValue(bool mixed)
{
    PushItemFlag(ImGuiItemFlags_MixedValue, mixed);
}

bool ImGui::InputFloat(const char* label, float* v, float step, float step_fast, int dec, ImGuiInputTextFlags flags)
{
    string_path Format;
    xr_sprintf(Format, "%%.%df", dec);
    return   InputFloat(label, v, step, step_fast, Format, flags);
}

bool ImGui::InputFloat2(const char* label, float v[2], int dec, ImGuiInputTextFlags flags)
{
    string_path Format;
    xr_sprintf(Format, "%%.%df", dec);
    return InputFloat2(label, v, Format, flags);
}

bool ImGui::InputFloat3(const char* label, float v[3], int dec, ImGuiInputTextFlags flags)
{
    string_path Format;
    xr_sprintf(Format, "%%.%df", dec);
    return  InputFloat3(label, v, Format, flags);
}

bool ImGui::InputFloat4(const char* label, float v[4], int dec, ImGuiInputTextFlags flags)
{
    string_path Format;
    xr_sprintf(Format, "%%.%df", dec);
    return  InputFloat4(label, v, Format, flags);
}
bool ImGui::BeginPopupModal(const char* name, bool* p_open, ImGuiWindowFlags flags, bool open_always)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;

    const ImGuiID id = window->GetID(name);
    if (!IsPopupOpen(id, ImGuiPopupFlags_None))
    {
        if (open_always)
        {
            OpenPopupEx(id);
        }
    }
   return  ImGui::BeginPopupModal(name, p_open, flags);
}

IMGUI_API bool ImGui::ArrowButton(const char* str_id, ImGuiDir dir, ImVec2 size, ImGuiButtonFlags flags)
{
    return ArrowButtonEx(str_id, dir, size, flags);
}
