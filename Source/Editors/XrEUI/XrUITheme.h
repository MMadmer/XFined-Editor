#pragma once

namespace XFinedTheme
{
enum class Preset : u32
{
	XFinedPurple = 0,
	Graphite,
	Count,
};

enum class ColorToken : u32
{
	Background = 0,
	Panel,
	Input,
	Border,
	Accent,
	AccentHovered,
	AccentActive,
	Selection,
	Text,
	Muted,
	Error,
	Warning,
	Success,
	Count,
};

XREUI_API void Initialize(float dpi_scale, bool multi_viewport);
XREUI_API void Apply(Preset preset);
XREUI_API Preset Current();
XREUI_API Preset Default();
XREUI_API bool IsValid(u32 preset);
XREUI_API bool TryParse(LPCSTR value, Preset& preset);
XREUI_API LPCSTR Key(Preset preset);
XREUI_API LPCSTR Name(Preset preset);
XREUI_API u32 Rgb(ColorToken token);
}
