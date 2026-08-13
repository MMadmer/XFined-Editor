#ifndef EThumbnailVisualH
#define EThumbnailVisualH
#pragma once

//------------------------------------------------------------------------------
// Offscreen preview renderer for engine visuals (.ogf)
//
// Both entry points draw a single model into a THUMB_WIDTH x THUMB_HEIGHT buffer
// and are safe to call from the middle of a frame (the ImGui pass included):
// every bit of device state they touch is saved and put back before returning.
//
// Pixel format: u32 X8R8G8B8 (alpha forced to 0xFF), same as
// EImageThumbnail::Pixels(). Rows run TOP-DOWN - `out[0]` is the top-left
// pixel - which is what a straight row-by-row upload
// (XFinedMCP::PixelsToTexture, D3DX) expects, so the picture comes out upright.
// EImageThumbnail uses the opposite row order: it stores its pixels flipped and
// EImageThumbnail::Update un-flips them while uploading, so handing this buffer
// to that class needs a VFlip in between.
//------------------------------------------------------------------------------

// Renders a model resolved through the editor FS ($level$ / $game_meshes$).
// Returns false if the device is not ready, the file is missing, its visual type
// is not supported by the editor, or the model failed to load.
ECORE_API bool RenderVisualThumbnail(LPCSTR visual_name, U32Vec& out);

// Same, but the OGF comes from a memory block (files packed into game archives
// are not on disk and are not registered in the editor FS). `debug_name` is only
// used for logging - the model pool key is generated internally.
ECORE_API bool RenderVisualThumbnailFromMemory(LPCSTR debug_name, const void* data, u32 size, U32Vec& out);

// Renders a library object (.object in $objects$). Around half of the shipped
// objects have no baked .thm, and this is the only way to preview those.
ECORE_API bool RenderObjectThumbnail(LPCSTR object_name, U32Vec& out);

// Same, from memory. A project's own .object lives outside the SDK root, so it
// is in neither $objects$ nor the editor FS at all - the by-name call above can
// never reach it and the caller has to hand over the bytes.
ECORE_API bool RenderObjectThumbnailFromMemory(LPCSTR debug_name, const void* data, u32 size, U32Vec& out);

//------------------------------------------------------------------------------
// Deferred queue - the safe way to ask for a thumbnail from UI code
//
// The two calls above have to close and reopen the caller's D3D9 scene, because
// GetRenderTargetData is illegal inside one. The ImGui pass runs inside the
// editor's scene AND inside a catch(...) (ui_main.cpp), so anything that throws
// mid-render escapes with the scene left unbalanced and the next frame dies on
// "Invalid call: HW.pDevice->BeginScene()".
//
// So UI code does not render: it queues a request and picks the result up on a
// later frame. FlushVisualThumbnailQueue runs once per frame from outside
// Begin()/End(), where no scene is open and the dance is not needed at all.
//------------------------------------------------------------------------------

// Queues a by-name request. `key` identifies the result; requesting a key that
// is already queued or already rendered does nothing.
ECORE_API void QueueVisualThumbnail(LPCSTR key, LPCSTR visual_name);

// Queues a library object (.object in $objects$). Roughly half of them ship no
// baked .thm, and those tiles would otherwise stay blank forever.
ECORE_API void QueueObjectThumbnail(LPCSTR key, LPCSTR object_name);

// Queues a request whose OGF bytes come from memory. The block is copied, so
// the caller may free it immediately.
ECORE_API void QueueVisualThumbnailFromMemory(LPCSTR key, const void* data, u32 size);

// Same for .object bytes - the project source reads its files off the disk
// itself, since nothing under the project root is in the editor FS.
ECORE_API void QueueObjectThumbnailFromMemory(LPCSTR key, const void* data, u32 size);

// Result poll. Returns true once the request finished and fills `out`; the
// entry is consumed. `failed` tells a finished-but-unrenderable request apart
// from one that is still waiting, so callers can stop asking.
ECORE_API bool TakeVisualThumbnail(LPCSTR key, U32Vec& out, bool& failed);

// Renders at most `max_requests` queued items. MUST be called with no D3D9
// scene open - the editor calls it right before EDevice->Begin().
ECORE_API void FlushVisualThumbnailQueue(u32 max_requests);

// Drops everything still queued (source switch, project close).
ECORE_API void ClearVisualThumbnailQueue();

#endif
