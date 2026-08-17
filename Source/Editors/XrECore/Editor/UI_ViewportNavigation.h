#pragma once

namespace ViewportNavigation
{
enum class Target : u32
{
	Perspective = 0,
	Front,
	Back,
	Left,
	Right,
	Top,
	Bottom,
	Count,
};

ECORE_API bool SetTarget(Target target);
ECORE_API Target CurrentTarget();
ECORE_API void ResetState();
ECORE_API bool TryParseTarget(LPCSTR value, Target& target);
ECORE_API LPCSTR TargetKey(Target target);
ECORE_API LPCSTR TargetName(Target target);

ECORE_API bool FrameAll();
ECORE_API bool FrameSelection();

// Draws the overlay and reports whether its panel owns the mouse position.
ECORE_API bool Draw(const ImVec2& canvas_position, const ImVec2& canvas_size);
}
