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

#define IMGUI_API XREUI_API
#define IMGUI_IMPL_API
#if defined(USE_DX11)
// What ImGui draws is a shader resource view under D3D11, not a texture object:
// the pixel shader binds an SRV, and every editor path that hands a picture to
// ImGui now produces one.
using ImTextureID = ID3D11ShaderResourceView*;
#else
using ImTextureID = IDirect3DBaseTexture9*;
#endif
#define ImTextureID ImTextureID
#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define IMGUI_INCLUDE_IMGUI_USER_H

#include "imgui.h"