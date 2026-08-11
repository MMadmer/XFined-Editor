#pragma once

// Built-in MCP endpoint.
//
// A tiny line-based JSON server on 127.0.0.1:28016, alive from the very first
// frame (project browser included). AI agents talk to it through the stdio
// bridge in tools/mcp/xfined_mcp.py. Requests are queued by the socket thread
// and executed on the main thread (device and scene are not thread-safe).
//
// Protocol: one JSON object per line in, one per line out.
//   {"cmd":"ping"}                -> {"ok":true,"name":"XFined Editor",...}
//   {"cmd":"state"}               -> project/scene/fps info
//   {"cmd":"screenshot_viewport"} -> {"ok":true,"png_base64":"..."}
//   {"cmd":"screenshot_editor"}   -> full editor window via PrintWindow,
//                                    works even when covered by other windows

// extension hook: the editor layer (LevelEditor) registers a handler for
// commands the core does not know — scene/selection/object inspection etc.
// Receives the command name and the raw request line (for arguments), fills
// the JSON response, returns true when it handled the command.
typedef bool (*TXFinedMCPHandler)(LPCSTR cmd, LPCSTR raw, xr_string& response);

class ECORE_API XFinedMCP
{
public:
	static void		Start		();		// spins up the listener thread
	static void		Stop		();
	static void		Pump		();		// main thread: execute queued requests
	static void		SetHandler	(TXFinedMCPHandler handler);

	// helper for handlers: extracts a string field from the raw request line
	static bool		GetArg		(LPCSTR raw, LPCSTR field, char* dst, u32 dst_size);

	// helper for handlers: encodes a D3D texture (IDirect3DTexture9*, passed as
	// void* so this header stays free of d3d) as a base64 PNG
	static bool		TextureToPngBase64(void* texture, xr_string& out);

	// helper for handlers: wraps a THUMB_WIDTH x THUMB_HEIGHT X8R8G8B8 pixel
	// buffer (what the thumbnail renderers produce) into a fresh managed D3D
	// texture. Caller owns the result and must Release it. Returns null on
	// failure or on a wrongly sized buffer.
	static void*	PixelsToTexture(const U32Vec& pixels);
};
