#pragma once
#include "..\..\XrCore\xrCore.h"
#ifdef XREUI_EXPORTS
#define XREUI_API __declspec(dllexport)
#else
#define XREUI_API __declspec(dllimport)
#endif
#include "XrUI.h"
#if defined(USE_DX11)
#include <d3d11.h>
#else
#include <d3d9.h>
#endif
#include "XrUIManager.h"

#include "imgui.h"
#include "XrUITheme.h"
