#include "stdafx.h"

namespace
{
float g_DpiScale = 1.f;
bool g_MultiViewport = false;
XFinedTheme::Preset g_Current = XFinedTheme::Preset::XFinedPurple;

ImVec4 Hex(u32 rgb, float alpha = 1.f)
{
	return ImVec4(
		float((rgb >> 16) & 0xff) / 255.f,
		float((rgb >> 8) & 0xff) / 255.f,
		float(rgb & 0xff) / 255.f,
		alpha);
}

struct ThemePalette
{
	u32 background;
	u32 panel;
	u32 input;
	u32 border;
	u32 accent;
	u32 accent_hovered;
	u32 accent_active;
	u32 selection;
	u32 text;
	u32 muted;
	u32 error;
	u32 warning;
	u32 success;
};

u32 Semantic(const ThemePalette& palette, XFinedTheme::ColorToken token)
{
	switch (token)
	{
	case XFinedTheme::ColorToken::Background:		return palette.background;
	case XFinedTheme::ColorToken::Panel:			return palette.panel;
	case XFinedTheme::ColorToken::Input:			return palette.input;
	case XFinedTheme::ColorToken::Border:			return palette.border;
	case XFinedTheme::ColorToken::Accent:			return palette.accent;
	case XFinedTheme::ColorToken::AccentHovered:	return palette.accent_hovered;
	case XFinedTheme::ColorToken::AccentActive:	return palette.accent_active;
	case XFinedTheme::ColorToken::Selection:		return palette.selection;
	case XFinedTheme::ColorToken::Text:			return palette.text;
	case XFinedTheme::ColorToken::Muted:			return palette.muted;
	case XFinedTheme::ColorToken::Error:			return palette.error;
	case XFinedTheme::ColorToken::Warning:			return palette.warning;
	case XFinedTheme::ColorToken::Success:			return palette.success;
	default:									return palette.text;
	}
}

void ApplyMetrics(ImGuiStyle& style)
{
	style.Alpha = 1.f;
	style.DisabledAlpha = .55f;
	style.WindowPadding = ImVec2(8.f, 6.f);
	style.WindowRounding = 4.f;
	style.WindowBorderSize = 1.f;
	style.WindowMinSize = ImVec2(32.f, 32.f);
	style.WindowTitleAlign = ImVec2(.5f, .5f);
	style.WindowMenuButtonPosition = ImGuiDir_Left;
	style.ChildRounding = 3.f;
	style.ChildBorderSize = 1.f;
	style.PopupRounding = 4.f;
	style.PopupBorderSize = 1.f;
	style.FramePadding = ImVec2(6.f, 3.f);
	style.FrameRounding = 3.f;
	style.FrameBorderSize = 0.f;
	style.ItemSpacing = ImVec2(6.f, 4.f);
	style.ItemInnerSpacing = ImVec2(5.f, 4.f);
	style.CellPadding = ImVec2(6.f, 3.f);
	style.TouchExtraPadding = ImVec2(0.f, 0.f);
	style.IndentSpacing = 18.f;
	style.ColumnsMinSpacing = 6.f;
	style.ScrollbarSize = 14.f;
	style.ScrollbarRounding = 3.f;
	style.ScrollbarPadding = 1.f;
	style.GrabMinSize = 10.f;
	style.GrabRounding = 3.f;
	style.LogSliderDeadzone = 4.f;
	style.ImageRounding = 3.f;
	style.ImageBorderSize = 0.f;
	style.TabRounding = 3.f;
	style.TabBorderSize = 0.f;
	style.TabCloseButtonMinWidthSelected = 0.f;
	style.TabCloseButtonMinWidthUnselected = 0.f;
	style.TabBarBorderSize = 1.f;
	style.TabBarOverlineSize = 2.f;
	style.TreeLinesSize = 1.f;
	style.TreeLinesRounding = 2.f;
	style.DragDropTargetRounding = 3.f;
	style.DragDropTargetBorderSize = 2.f;
	style.DragDropTargetPadding = 3.f;
	style.ColorButtonPosition = ImGuiDir_Right;
	style.ButtonTextAlign = ImVec2(.5f, .5f);
	style.SelectableTextAlign = ImVec2(0.f, .5f);
	style.InputTextCursorSize = 1.f;
	style.SeparatorSize = 1.f;
	style.DisplayWindowPadding = ImVec2(19.f, 19.f);
	style.DisplaySafeAreaPadding = ImVec2(3.f, 3.f);
	style.DockingSeparatorSize = 2.f;
	style.MouseCursorScale = 1.f;
	style.AntiAliasedLines = true;
	style.AntiAliasedLinesUseTex = true;
	style.AntiAliasedFill = true;
	style.CurveTessellationTol = 1.25f;
	style.CircleTessellationMaxError = .3f;
}

void ApplyDarkPalette(ImGuiStyle& style, const ThemePalette& palette)
{
	ImVec4* colors = style.Colors;
	colors[ImGuiCol_Text] = Hex(Semantic(palette, XFinedTheme::ColorToken::Text));
	colors[ImGuiCol_TextDisabled] = Hex(Semantic(palette, XFinedTheme::ColorToken::Muted));
	colors[ImGuiCol_WindowBg] = Hex(Semantic(palette, XFinedTheme::ColorToken::Background));
	colors[ImGuiCol_ChildBg] = Hex(Semantic(palette, XFinedTheme::ColorToken::Panel));
	colors[ImGuiCol_PopupBg] = Hex(0x24232a, .98f);
	colors[ImGuiCol_Border] = Hex(Semantic(palette, XFinedTheme::ColorToken::Border), .9f);
	colors[ImGuiCol_BorderShadow] = Hex(0x09090b, 0.f);
	colors[ImGuiCol_FrameBg] = Hex(Semantic(palette, XFinedTheme::ColorToken::Input));
	colors[ImGuiCol_FrameBgHovered] = Hex(palette.selection);
	colors[ImGuiCol_FrameBgActive] = Hex(palette.accent_hovered);
	colors[ImGuiCol_TitleBg] = Hex(0x18171d);
	colors[ImGuiCol_TitleBgActive] = Hex(0x282531);
	colors[ImGuiCol_TitleBgCollapsed] = Hex(0x18171d, .8f);
	colors[ImGuiCol_MenuBarBg] = Hex(0x222126);
	colors[ImGuiCol_ScrollbarBg] = Hex(0x17161a, .75f);
	colors[ImGuiCol_ScrollbarGrab] = Hex(0x3d3945);
	colors[ImGuiCol_ScrollbarGrabHovered] = Hex(palette.selection);
	colors[ImGuiCol_ScrollbarGrabActive] = Hex(palette.accent);
	colors[ImGuiCol_CheckMark] = Hex(palette.accent_active);
	colors[ImGuiCol_CheckboxSelectedBg] = Hex(palette.accent, .55f);
	colors[ImGuiCol_SliderGrab] = Hex(palette.accent);
	colors[ImGuiCol_SliderGrabActive] = Hex(palette.accent_active);
	colors[ImGuiCol_Button] = Hex(0x3a3742);
	colors[ImGuiCol_ButtonHovered] = Hex(palette.accent_hovered);
	colors[ImGuiCol_ButtonActive] = Hex(palette.accent);
	colors[ImGuiCol_Header] = Hex(0x3b3747);
	colors[ImGuiCol_HeaderHovered] = Hex(palette.accent_hovered);
	colors[ImGuiCol_HeaderActive] = Hex(palette.accent);
	colors[ImGuiCol_Separator] = Hex(0x403c49);
	colors[ImGuiCol_SeparatorHovered] = Hex(palette.accent_hovered);
	colors[ImGuiCol_SeparatorActive] = Hex(palette.accent_active);
	colors[ImGuiCol_ResizeGrip] = Hex(palette.accent, .25f);
	colors[ImGuiCol_ResizeGripHovered] = Hex(palette.accent, .67f);
	colors[ImGuiCol_ResizeGripActive] = Hex(palette.accent_active, .95f);
	colors[ImGuiCol_InputTextCursor] = Hex(palette.accent_active);
	colors[ImGuiCol_TabHovered] = Hex(palette.accent_hovered);
	colors[ImGuiCol_Tab] = Hex(0x24232a);
	colors[ImGuiCol_TabSelected] = Hex(0x403b56);
	colors[ImGuiCol_TabSelectedOverline] = Hex(palette.accent_active);
	colors[ImGuiCol_TabDimmed] = Hex(0x1d1c22);
	colors[ImGuiCol_TabDimmedSelected] = Hex(0x302d3b);
	colors[ImGuiCol_TabDimmedSelectedOverline] = Hex(palette.accent, .75f);
	colors[ImGuiCol_DockingPreview] = Hex(palette.accent, .7f);
	colors[ImGuiCol_DockingEmptyBg] = Hex(0x17161a);
	colors[ImGuiCol_PlotLines] = Hex(0xb8b3c4);
	colors[ImGuiCol_PlotLinesHovered] = Hex(Semantic(palette, XFinedTheme::ColorToken::Warning));
	colors[ImGuiCol_PlotHistogram] = Hex(palette.accent_active);
	colors[ImGuiCol_PlotHistogramHovered] = Hex(0xc5bfff);
	colors[ImGuiCol_TableHeaderBg] = Hex(0x302d38);
	colors[ImGuiCol_TableBorderStrong] = Hex(0x494451);
	colors[ImGuiCol_TableBorderLight] = Hex(0x35323c);
	colors[ImGuiCol_TableRowBg] = Hex(0x202025, 0.f);
	colors[ImGuiCol_TableRowBgAlt] = Hex(0xffffff, .025f);
	colors[ImGuiCol_TextLink] = Hex(palette.accent_active);
	colors[ImGuiCol_TextSelectedBg] = Hex(Semantic(palette, XFinedTheme::ColorToken::Selection), .55f);
	colors[ImGuiCol_TreeLines] = Hex(palette.border);
	colors[ImGuiCol_DragDropTarget] = Hex(Semantic(palette, XFinedTheme::ColorToken::Warning), .9f);
	colors[ImGuiCol_DragDropTargetBg] = Hex(Semantic(palette, XFinedTheme::ColorToken::Warning), .16f);
	colors[ImGuiCol_UnsavedMarker] = Hex(Semantic(palette, XFinedTheme::ColorToken::Warning));
	colors[ImGuiCol_NavCursor] = Hex(palette.accent_active);
	colors[ImGuiCol_NavWindowingHighlight] = Hex(0xf2effb, .7f);
	colors[ImGuiCol_NavWindowingDimBg] = Hex(0x0c0b0e, .45f);
	colors[ImGuiCol_ModalWindowDimBg] = Hex(0x0c0b0e, .68f);
}

ThemePalette PaletteFor(XFinedTheme::Preset preset)
{
	if (preset == XFinedTheme::Preset::Graphite)
		return {
			0x202025, 0x1c1b20, 0x333333, 0x3d3947,
			0x727782, 0x50545d, 0x9399a5, 0x454951,
			0xf2effb, 0x8e8a99, 0xe06c75, 0xe5c07b, 0x98c379,
		};
	return {
		0x202025, 0x1c1b20, 0x333333, 0x3d3947,
		0x7971bd, 0x554f72, 0xa79fed, 0x464150,
		0xf2effb, 0x8e8a99, 0xe06c75, 0xe5c07b, 0x98c379,
	};
}
}

void XFinedTheme::Initialize(float dpi_scale, bool multi_viewport)
{
	g_DpiScale = dpi_scale > 0.f ? dpi_scale : 1.f;
	g_MultiViewport = multi_viewport;
	Apply(g_Current);
}

void XFinedTheme::Apply(Preset preset)
{
	if (!IsValid(u32(preset)))
		preset = Default();
	g_Current = preset;
	if (!ImGui::GetCurrentContext())
		return;

	ImGuiStyle style;
	ApplyMetrics(style);
	ApplyDarkPalette(style, PaletteFor(preset));
	if (g_MultiViewport)
	{
		style.WindowRounding = 0.f;
		style.Colors[ImGuiCol_WindowBg].w = 1.f;
	}
	if (g_DpiScale != 1.f)
		style.ScaleAllSizes(g_DpiScale);
	ImGui::GetStyle() = style;
}

XFinedTheme::Preset XFinedTheme::Current()
{
	return g_Current;
}

XFinedTheme::Preset XFinedTheme::Default()
{
	return Preset::XFinedPurple;
}

bool XFinedTheme::IsValid(u32 preset)
{
	return preset < u32(Preset::Count);
}

bool XFinedTheme::TryParse(LPCSTR value, Preset& preset)
{
	if (!value || !value[0])
		return false;
	for (u32 i = 0; i < u32(Preset::Count); ++i)
	{
		const Preset candidate = Preset(i);
		if (0 == _stricmp(value, Key(candidate)) || 0 == _stricmp(value, Name(candidate)))
		{
			preset = candidate;
			return true;
		}
	}
	return false;
}

LPCSTR XFinedTheme::Key(Preset preset)
{
	switch (preset)
	{
	case Preset::XFinedPurple:	return "xfined-purple";
	case Preset::Graphite:		return "graphite";
	default:					return "xfined-purple";
	}
}

LPCSTR XFinedTheme::Name(Preset preset)
{
	switch (preset)
	{
	case Preset::XFinedPurple:	return "XFined Purple";
	case Preset::Graphite:		return "Graphite";
	default:					return "XFined Purple";
	}
}

u32 XFinedTheme::Rgb(ColorToken token)
{
	return Semantic(PaletteFor(g_Current), token);
}
