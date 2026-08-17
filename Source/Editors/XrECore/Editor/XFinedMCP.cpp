#include "stdafx.h"
#include "XFinedMCP.h"
#include "EditorProject.h"
#include "UI_ToolsCustom.h"
#include "ui_main.h"
#include <climits>
#include <cstdlib>
#if defined(USE_DX11)
#include "EDX11Utils.h"
// the png encoder itself is compiled into RedImageTool, which XrECore links -
// this only pulls the declarations
#include "StbImage\stb_image_write.h"
#endif
// windows.h in the PCH already pulled the legacy winsock.h — its 1.1 API is
// all we need, and including winsock2.h on top would clash with it
#pragma comment(lib, "ws2_32.lib")

static const u16 kPort = 28016;
static constexpr int kShutdownBoth = 2;
static const DWORD kDefaultRequestTimeoutMs = 180000;
static const u32 kMaxClients = 16;
static const u32 kMaxQueuedRequests = 64;
static const size_t kMaxRequestBytes = 1024 * 1024;

//------------------------------------------------------------------------------
// request queue: socket workers produce, main thread consumes
//------------------------------------------------------------------------------
// The client and queue/batch each own one reference. The event dies only after
// both sides have observed completion or cancellation.
struct SMCPRequest
{
	xr_string		cmd;
	xr_string		raw;		// full request line, for handler arguments
	xr_string		response;
	HANDLE			done;
	volatile LONG	state;
	volatile LONG	references;

	enum EState
	{
		Queued,
		Executing,
		Completed,
		Cancelled
	};

	SMCPRequest() : done(::CreateEventA(NULL, FALSE, FALSE, NULL)), state(Queued), references(1) {}
	~SMCPRequest() { if (done) ::CloseHandle(done); }

	void AddRef() { ::InterlockedIncrement(&references); }
	void Release()
	{
		if (!::InterlockedDecrement(&references))
		{
			SMCPRequest* self = this;
			xr_delete(self);
		}
	}
	bool Cancel()
	{
		return Queued == ::InterlockedCompareExchange(&state, Cancelled, Queued);
	}
};

static TXFinedMCPHandler	s_Handler = 0;
void XFinedMCP::SetHandler(TXFinedMCPHandler handler) { s_Handler = handler; }

namespace
{
static constexpr u32 kMaxJsonDepth = 64;

static void SkipJsonWhitespace(const char*& cursor)
{
	while (' ' == *cursor || '\t' == *cursor || '\r' == *cursor || '\n' == *cursor)
		++cursor;
}

static int JsonHexDigit(char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static bool ParseJsonHexQuad(const char*& cursor, u32& value)
{
	value = 0;
	for (u32 i = 0; i < 4; ++i)
	{
		const int digit = JsonHexDigit(*cursor);
		if (digit < 0)
			return false;
		value = (value << 4) | u32(digit);
		++cursor;
	}
	return true;
}

static bool AppendJsonCodePoint(u32 code_point, xr_string* output)
{
	if (!code_point || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF))
		return false;
	if (!output)
		return true;

	char utf8[4];
	u32 size = 0;
	if (code_point < 0x80)
	{
		utf8[size++] = char(code_point);
	}
	else if (code_point < 0x800)
	{
		utf8[size++] = char(0xC0 | (code_point >> 6));
		utf8[size++] = char(0x80 | (code_point & 0x3F));
	}
	else if (code_point < 0x10000)
	{
		utf8[size++] = char(0xE0 | (code_point >> 12));
		utf8[size++] = char(0x80 | ((code_point >> 6) & 0x3F));
		utf8[size++] = char(0x80 | (code_point & 0x3F));
	}
	else
	{
		utf8[size++] = char(0xF0 | (code_point >> 18));
		utf8[size++] = char(0x80 | ((code_point >> 12) & 0x3F));
		utf8[size++] = char(0x80 | ((code_point >> 6) & 0x3F));
		utf8[size++] = char(0x80 | (code_point & 0x3F));
	}
	output->append(utf8, size);
	return true;
}

static bool AppendRawJsonUtf8(const char*& cursor, xr_string* output)
{
	const char* start = cursor;
	const u32 lead = static_cast<unsigned char>(*cursor);
	u32 size = 0;
	u32 code_point = 0;
	u32 minimum = 0;
	if (lead >= 0xC2 && lead <= 0xDF)
	{
		size = 2;
		code_point = lead & 0x1F;
		minimum = 0x80;
	}
	else if (lead >= 0xE0 && lead <= 0xEF)
	{
		size = 3;
		code_point = lead & 0x0F;
		minimum = 0x800;
	}
	else if (lead >= 0xF0 && lead <= 0xF4)
	{
		size = 4;
		code_point = lead & 0x07;
		minimum = 0x10000;
	}
	else
	{
		return false;
	}

	for (u32 i = 1; i < size; ++i)
	{
		const u32 byte = static_cast<unsigned char>(cursor[i]);
		if ((byte & 0xC0) != 0x80)
			return false;
		code_point = (code_point << 6) | (byte & 0x3F);
	}
	if (code_point < minimum || code_point > 0x10FFFF ||
		(code_point >= 0xD800 && code_point <= 0xDFFF))
	{
		return false;
	}

	if (output)
		output->append(start, size);
	cursor += size;
	return true;
}

static bool ParseJsonString(const char*& cursor, xr_string* output)
{
	if ('"' != *cursor)
		return false;
	++cursor;
	while (*cursor)
	{
		const u32 byte = static_cast<unsigned char>(*cursor);
		if ('"' == byte)
		{
			++cursor;
			return true;
		}
		if ('\\' == byte)
		{
			++cursor;
			const char escape = *cursor;
			if (!escape)
				return false;
			++cursor;
			switch (escape)
			{
			case '"': if (output) output->push_back('"'); break;
			case '\\': if (output) output->push_back('\\'); break;
			case '/':  if (output) output->push_back('/');  break;
			case 'b':  if (output) output->push_back('\b'); break;
			case 'f':  if (output) output->push_back('\f'); break;
			case 'n':  if (output) output->push_back('\n'); break;
			case 'r':  if (output) output->push_back('\r'); break;
			case 't':  if (output) output->push_back('\t'); break;
			case 'u':
				{
					u32 code_point = 0;
					if (!ParseJsonHexQuad(cursor, code_point))
						return false;
					if (code_point >= 0xD800 && code_point <= 0xDBFF)
					{
						if ('\\' != *cursor)
							return false;
						++cursor;
						if ('u' != *cursor)
							return false;
						++cursor;
						u32 low = 0;
						if (!ParseJsonHexQuad(cursor, low) || low < 0xDC00 || low > 0xDFFF)
							return false;
						code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
					}
					else if (code_point >= 0xDC00 && code_point <= 0xDFFF)
					{
						return false;
					}
					if (!AppendJsonCodePoint(code_point, output))
						return false;
					break;
				}
			default:
				return false;
			}
			continue;
		}
		if (byte < 0x20)
			return false;
		if (byte < 0x80)
		{
			if (output)
				output->push_back(char(byte));
			++cursor;
			continue;
		}
		if (!AppendRawJsonUtf8(cursor, output))
			return false;
	}
	return false;
}

static bool SkipJsonValue(const char*& cursor, u32 depth);

static bool SkipJsonArray(const char*& cursor, u32 depth)
{
	if (depth >= kMaxJsonDepth || '[' != *cursor)
		return false;
	++cursor;
	SkipJsonWhitespace(cursor);
	if (']' == *cursor)
	{
		++cursor;
		return true;
	}
	for (;;)
	{
		if (!SkipJsonValue(cursor, depth + 1))
			return false;
		SkipJsonWhitespace(cursor);
		if (']' == *cursor)
		{
			++cursor;
			return true;
		}
		if (',' != *cursor)
			return false;
		++cursor;
		SkipJsonWhitespace(cursor);
	}
}

static bool SkipJsonObject(const char*& cursor, u32 depth)
{
	if (depth >= kMaxJsonDepth || '{' != *cursor)
		return false;
	++cursor;
	SkipJsonWhitespace(cursor);
	if ('}' == *cursor)
	{
		++cursor;
		return true;
	}
	for (;;)
	{
		if (!ParseJsonString(cursor, 0))
			return false;
		SkipJsonWhitespace(cursor);
		if (':' != *cursor)
			return false;
		++cursor;
		SkipJsonWhitespace(cursor);
		if (!SkipJsonValue(cursor, depth + 1))
			return false;
		SkipJsonWhitespace(cursor);
		if ('}' == *cursor)
		{
			++cursor;
			return true;
		}
		if (',' != *cursor)
			return false;
		++cursor;
		SkipJsonWhitespace(cursor);
	}
}

static bool SkipJsonNumber(const char*& cursor)
{
	if ('-' == *cursor)
		++cursor;
	if ('0' == *cursor)
	{
		++cursor;
	}
	else
	{
		if (*cursor < '1' || *cursor > '9')
			return false;
		do { ++cursor; } while (*cursor >= '0' && *cursor <= '9');
	}
	if ('.' == *cursor)
	{
		++cursor;
		if (*cursor < '0' || *cursor > '9')
			return false;
		do { ++cursor; } while (*cursor >= '0' && *cursor <= '9');
	}
	if ('e' == *cursor || 'E' == *cursor)
	{
		++cursor;
		if ('+' == *cursor || '-' == *cursor)
			++cursor;
		if (*cursor < '0' || *cursor > '9')
			return false;
		do { ++cursor; } while (*cursor >= '0' && *cursor <= '9');
	}
	return true;
}

static bool SkipJsonValue(const char*& cursor, u32 depth)
{
	if ('"' == *cursor)
		return ParseJsonString(cursor, 0);
	if ('{' == *cursor)
		return SkipJsonObject(cursor, depth);
	if ('[' == *cursor)
		return SkipJsonArray(cursor, depth);
	if (!strncmp(cursor, "true", 4))
	{
		cursor += 4;
		return true;
	}
	if (!strncmp(cursor, "false", 5))
	{
		cursor += 5;
		return true;
	}
	if (!strncmp(cursor, "null", 4))
	{
		cursor += 4;
		return true;
	}
	return SkipJsonNumber(cursor);
}
}

bool XFinedMCP::GetArg(LPCSTR raw, LPCSTR field, char* dst, u32 dst_size)
{
	if (!dst || !dst_size)
		return false;
	dst[0] = 0;
	if (!raw || !field || !*field)
		return false;

	const char* cursor = raw;
	SkipJsonWhitespace(cursor);
	if ('{' != *cursor)
		return false;
	++cursor;
	SkipJsonWhitespace(cursor);

	bool found = false;
	xr_string value;
	if ('}' != *cursor)
	{
		for (;;)
		{
			xr_string key;
			if (!ParseJsonString(cursor, &key))
				return false;
			SkipJsonWhitespace(cursor);
			if (':' != *cursor)
				return false;
			++cursor;
			SkipJsonWhitespace(cursor);

			if (key == field)
			{
				if (found || '"' != *cursor)
					return false;
				xr_string parsed;
				if (!ParseJsonString(cursor, &parsed))
					return false;
				value.swap(parsed);
				found = true;
			}
			else if (!SkipJsonValue(cursor, 1))
			{
				return false;
			}

			SkipJsonWhitespace(cursor);
			if ('}' == *cursor)
				break;
			if (',' != *cursor)
				return false;
			++cursor;
			SkipJsonWhitespace(cursor);
		}
	}
	++cursor;
	SkipJsonWhitespace(cursor);
	if (*cursor || !found || value.size() >= dst_size)
		return false;

	memcpy(dst, value.data(), value.size());
	dst[value.size()] = 0;
	return true;
}

struct SMCPClient
{
	SOCKET socket;
	HANDLE thread;

	explicit SMCPClient(SOCKET value) : socket(value), thread(0) {}
};

static CRITICAL_SECTION			s_Lock;
static bool					s_LockReady = false;
static xr_vector<SMCPRequest*>	s_Queue;
static xr_vector<SMCPClient*>	s_Clients;
static SOCKET					s_Listen = INVALID_SOCKET;
static HANDLE					s_AcceptThread = 0;
static HANDLE					s_StopEvent = 0;
static HANDLE					s_QueueEvent = 0;
static volatile LONG			s_Run = 0;
static bool					s_WsaReady = false;
static DWORD					s_RequestTimeoutMs = kDefaultRequestTimeoutMs;

static bool IsRunning()
{
	return 0 != ::InterlockedCompareExchange(&s_Run, 0, 0);
}

//------------------------------------------------------------------------------
// helpers
//------------------------------------------------------------------------------
static void Base64(const u8* data, u32 size, xr_string& out)
{
	static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	out.clear();
	out.reserve((size + 2) / 3 * 4);
	for (u32 i = 0; i < size; i += 3)
	{
		u32 b = data[i] << 16;
		if (i + 1 < size) b |= data[i + 1] << 8;
		if (i + 2 < size) b |= data[i + 2];
		out += tbl[(b >> 18) & 63];
		out += tbl[(b >> 12) & 63];
		out += (i + 1 < size) ? tbl[(b >> 6) & 63] : '=';
		out += (i + 2 < size) ? tbl[b & 63] : '=';
	}
}

static void JsonEscapePath(const char* src, char* dst, u32 dst_size)
{
	u32 o = 0;
	for (const char* p = src; *p && o + 2 < dst_size; ++p)
		dst[o++] = (*p == '\\') ? '/' : *p;
	dst[o] = 0;
}

#if defined(USE_DX11)
// stb hands the encoded stream over in chunks
static void PngSink(void* ctx, void* data, int size)
{
	xr_vector<u8>* buf = (xr_vector<u8>*)ctx;
	buf->insert(buf->end(), (const u8*)data, (const u8*)data + size);
}

// Encodes a B8G8R8A8 block - the layout the readback and the thumbnail renderers
// produce - as a base64 png. Row order is left alone: the D3D9 encoder was fed
// top-down rows as well.
static bool PixelsToPngBase64(const u32* px, u32 w, u32 h, xr_string& out)
{
	if (!px || !w || !h)	return false;

	// stb writes bytes in memory order, so R and B trade places; alpha is forced
	// opaque because the old X8R8G8B8 surfaces carried no meaningful alpha either
	U32Vec rgba(size_t(w) * size_t(h));
	for (size_t i = 0, n = rgba.size(); i < n; ++i)
	{
		const u32 c = px[i];
		rgba[i] = 0xff000000 | (c & 0x0000ff00) | ((c & 0x00ff0000) >> 16) | ((c & 0x000000ff) << 16);
	}

	xr_vector<u8> png;
	if (!stbi_write_png_to_func(&PngSink, &png, int(w), int(h), 4, rgba.data(), int(w * sizeof(u32))) || png.empty())
		return false;

	Base64(png.data(), u32(png.size()), out);
	return true;
}

// Reads a texture back and encodes it. MSAA targets cannot be copied straight
// into a staging texture, but nothing in the editor renders previews multisampled.
static bool TextureToPngBase64_DX11(ID3D11Texture2D* tex, xr_string& out)
{
	U32Vec	px;
	u32		w = 0, h = 0;
	if (!DX11ReadbackBGRA(tex, px, w, h))	return false;
	return PixelsToPngBase64(px.data(), w, h, out);
}
#else
static bool SurfaceToPngBase64(IDirect3DSurface9* surf, xr_string& out)
{
	ID3DXBuffer* buf = 0;
	if (FAILED(D3DXSaveSurfaceToFileInMemory(&buf, D3DXIFF_PNG, surf, NULL, NULL)) || !buf)
		return false;
	Base64((const u8*)buf->GetBufferPointer(), buf->GetBufferSize(), out);
	buf->Release();
	return true;
}
#endif

bool XFinedMCP::TextureToPngBase64(void* texture, xr_string& out)
{
#if defined(USE_DX11)
	// callers hand over what ImGui draws, i.e. a shader resource view - walk back
	// to the texture behind it
	ID3D11ShaderResourceView* view = (ID3D11ShaderResourceView*)texture;
	if (!view) return false;

	ID3D11Resource* res = 0;
	view->GetResource(&res);
	if (!res) return false;

	ID3D11Texture2D* tex = 0;
	bool ok = false;
	if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
	{
		ok = TextureToPngBase64_DX11(tex, out);
		_RELEASE(tex);
	}
	_RELEASE(res);
	return ok;
#else
	IDirect3DTexture9* tex = (IDirect3DTexture9*)texture;
	if (!tex) return false;
	IDirect3DSurface9* surf = 0;
	if (FAILED(tex->GetSurfaceLevel(0, &surf)) || !surf)
		return false;
	const bool ok = SurfaceToPngBase64(surf, out);
	surf->Release();
	return ok;
#endif
}

void* XFinedMCP::PixelsToTexture(const U32Vec& pixels)
{
	if (pixels.size() != u32(THUMB_WIDTH) * u32(THUMB_HEIGHT))
		return 0;

#if defined(USE_DX11)
	// what the caller wants is something ImGui can draw and Release, and that is
	// a view; the helper drops the texture as soon as the view owns it
	return DX11TextureFromPixels(pixels.data(), THUMB_WIDTH, THUMB_HEIGHT);
#else
	IDirect3DTexture9* tex = 0;
	if (FAILED(HW.pDevice->CreateTexture(THUMB_WIDTH, THUMB_HEIGHT, 1, 0, D3DFMT_X8R8G8B8,
										 D3DPOOL_MANAGED, &tex, 0)) || !tex)
		return 0;

	D3DLOCKED_RECT rect;
	if (FAILED(tex->LockRect(0, &rect, 0, 0)))
	{
		tex->Release();
		return 0;
	}
	// row by row: the destination pitch is not the row size in general
	for (int y = 0; y < THUMB_HEIGHT; ++y)
		memcpy((u8*)rect.pBits + rect.Pitch * y, &pixels[u32(y) * THUMB_WIDTH], THUMB_WIDTH * sizeof(u32));
	tex->UnlockRect(0);
	return tex;
#endif
}

void XFinedMCP::ReleaseTexture(void* texture)
{
	if (!texture) return;
#if defined(USE_DX11)
	((ID3D11ShaderResourceView*)texture)->Release();
#else
	((IDirect3DBaseTexture9*)texture)->Release();
#endif
}

#if !defined(USE_DX11)
// captures the editor window through GDI — works even when fully covered
static bool WindowToPngBase64(HWND wnd, xr_string& out)
{
	RECT rc;
	if (!::GetClientRect(wnd, &rc)) return false;
	const int w = rc.right - rc.left, h = rc.bottom - rc.top;
	if (w <= 0 || h <= 0) return false;

	HDC wnd_dc = ::GetDC(wnd);
	HDC mem_dc = ::CreateCompatibleDC(wnd_dc);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;	// top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = 0;
	HBITMAP bmp = ::CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	bool ok = false;
	if (bmp && bits)
	{
		HGDIOBJ old = ::SelectObject(mem_dc, bmp);
		// PW_RENDERFULLCONTENT (2) grabs D3D-composited content on Win8.1+
		ok = !!::PrintWindow(wnd, mem_dc, 2);
		::SelectObject(mem_dc, old);
		if (ok)
		{
			// wrap the pixels into a D3D surface so D3DX encodes the png
			IDirect3DSurface9* surf = 0;
			if (SUCCEEDED(HW.pDevice->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &surf, NULL)) && surf)
			{
				D3DLOCKED_RECT lr;
				if (SUCCEEDED(surf->LockRect(&lr, NULL, 0)))
				{
					for (int y = 0; y < h; ++y)
						CopyMemory((u8*)lr.pBits + y * lr.Pitch, (u8*)bits + y * w * 4, w * 4);
					surf->UnlockRect();
					ok = SurfaceToPngBase64(surf, out);
				}
				else ok = false;
				surf->Release();
			}
			else ok = false;
		}
	}
	if (bmp) ::DeleteObject(bmp);
	::DeleteDC(mem_dc);
	::ReleaseDC(wnd, wnd_dc);
	return ok;
}
#endif	//	!USE_DX11

//------------------------------------------------------------------------------
// command execution — main thread only
//------------------------------------------------------------------------------
static void Execute(SMCPRequest& r)
{
	char tmp[1024];
	if (r.cmd == "ping")
	{
		r.response = "{\"ok\":true,\"name\":\"XFined Editor\",\"version\":\"1.0\"}";
	}
	else if (r.cmd == "state")
	{
		char proj[MAX_PATH] = {}, scene[MAX_PATH] = {};
		JsonEscapePath(EditorProject::Active() ? EditorProject::Root() : "", proj, sizeof(proj));
		if (Tools && Tools->m_LastFileName.size())
			JsonEscapePath(Tools->m_LastFileName.c_str(), scene, sizeof(scene));
		const float fps = (EDevice->fTimeDelta > EPS_S) ? 1.f / EDevice->fTimeDelta : 0.f;
		sprintf_s(tmp,
			"{\"ok\":true,\"project_active\":%s,\"project\":\"%s\",\"project_name\":\"%s\",\"scene\":\"%s\",\"fps\":%.0f}",
			EditorProject::Active() ? "true" : "false", proj,
			EditorProject::Active() ? EditorProject::Name() : "", scene, fps);
		r.response = tmp;
	}
	else if (r.cmd == "screenshot_viewport")
	{
		xr_string b64;
		bool ok = false;
		if (UI && UI->RT->pSurface)
		{
#if defined(USE_DX11)
			// no surface to fetch under D3D11 - the render target texture itself
			// is what gets copied into a staging texture and mapped
			ok = TextureToPngBase64_DX11(UI->RT->pSurface, b64);
#else
			IDirect3DSurface9* surf = 0;
			if (SUCCEEDED(((IDirect3DTexture9*)UI->RT->pSurface)->GetSurfaceLevel(0, &surf)) && surf)
			{
				ok = SurfaceToPngBase64(surf, b64);
				surf->Release();
			}
#endif
		}
		if (ok)	{ r.response = "{\"ok\":true,\"png_base64\":\""; r.response += b64; r.response += "\"}"; }
		else	r.response = "{\"ok\":false,\"error\":\"viewport RT unavailable\"}";
	}
	else if (r.cmd == "screenshot_editor")
	{
		xr_string b64;
#if defined(USE_DX11)
		// Straight from the presented frame. GDI shows a flip-model swap chain
		// as blank, so PrintWindow is not an option any more. Arming first means
		// the very first request after an idle spell answers "not ready".
		U32Vec	px;
		u32		w = 0, h = 0;
		DX11ArmFrameCapture();
		// the two ways this fails are nothing alike, and one message for both
		// sent the last debugging session chasing the wrong one
		if (!DX11GetFrameCapture(px, w, h))
			r.response = "{\"ok\":false,\"error\":\"capture just armed, no frame mirrored yet - ask again\"}";
		else if (!PixelsToPngBase64(px.data(), w, h, b64))
		{
			char msg[160];
			sprintf_s(msg, "{\"ok\":false,\"error\":\"png encode failed for %ux%u\"}", w, h);
			r.response = msg;
		}
		else
		{
			r.response = "{\"ok\":true,\"png_base64\":\"";
			r.response += b64;
			r.response += "\"}";
		}
#else
		// resolved by window class — m_hWnd visibility varies across the tree
		HWND wnd = ::FindWindowA("XFined Editor", NULL);
		if (wnd && WindowToPngBase64(wnd, b64))
		{
			r.response = "{\"ok\":true,\"png_base64\":\"";
			r.response += b64;
			r.response += "\"}";
		}
		else r.response = "{\"ok\":false,\"error\":\"PrintWindow capture failed\"}";
#endif
	}
	else if (r.cmd == "list_projects")
	{
		xr_string arr;
		EditorProject::ListRecentJson(arr);
		r.response = "{\"ok\":true,\"projects\":";
		r.response += arr;
		r.response += "}";
	}
	else if (r.cmd == "open_project")
	{
		char path[MAX_PATH];
		if (!XFinedMCP::GetArg(r.raw.c_str(), "path", path, sizeof(path)))
			r.response = "{\"ok\":false,\"error\":\"missing 'path' argument\"}";
		else
		{
			// forward slashes are welcome over the wire
			for (char* p = path; *p; ++p) if (*p == '/') *p = '\\';
			if (EditorProject::Active() && 0 == _stricmp(EditorProject::Root(), path))
				r.response = "{\"ok\":true,\"note\":\"already open\"}";
			else
			{
				if (EditorProject::Open(path))
					r.response = "{\"ok\":true}";
				else
				{
					r.response = "{\"ok\":false,\"error\":\"";
					r.response += EditorProject::OpenError()[0] ? EditorProject::OpenError() : "can't open project";
					r.response += "\"}";
				}
			}
		}
	}
	else if (r.cmd == "open_scene")
	{
		char scene[MAX_PATH];
		if (!EditorProject::Active())
			r.response = "{\"ok\":false,\"error\":\"no active project\"}";
		else if (!XFinedMCP::GetArg(r.raw.c_str(), "scene", scene, sizeof(scene)))
			r.response = "{\"ok\":false,\"error\":\"missing 'scene' argument\"}";
		else
		{
			for (char* p = scene; *p; ++p) if (*p == '/') *p = '\\';
			// COMMAND_LOAD does FS.r_open on the string as-is and returns false
			// silently on a miss, so a bare scene name must be resolved against
			// the project's $maps$ before it goes in - "worked" bare names were
			// really files lying in the editor's CWD
			string_path full;
			if (strchr(scene, ':'))		strcpy_s(full, scene);
			else						FS.update_path(full, "$maps$", scene);
			if (FS.exist(full))
			{
				// COMMAND_LOAD refuses a file that is not a scene (and says so in
				// the log) instead of loading it; report what actually happened
				// rather than a blanket ok
				const bool loaded = !!(BOOL)ExecCommand(COMMAND_LOAD, xr_string(full));
				if (loaded)
					r.response = "{\"ok\":true}";
				else
				{
					char esc[MAX_PATH * 2];
					JsonEscapePath(full, esc, sizeof(esc));
					r.response = "{\"ok\":false,\"error\":\"not a loadable scene: ";
					r.response += esc;
					r.response += "\"}";
				}
			}
			else
			{
				char esc[MAX_PATH * 2];
				JsonEscapePath(full, esc, sizeof(esc));
				r.response = "{\"ok\":false,\"error\":\"scene not found: ";
				r.response += esc;
				r.response += "\"}";
			}
		}
	}
	else if (s_Handler && s_Handler(r.cmd.c_str(), r.raw.c_str(), r.response))
	{
		// handled by the editor-layer inspector
	}
	else
	{
		r.response = "{\"ok\":false,\"error\":\"unknown command\"}";
	}
}

static SMCPRequest* TakeQueuedRequest(bool progress_only)
{
	if (!s_LockReady)
		return 0;

	SMCPRequest* request = 0;
	bool requests_remain = false;
	::EnterCriticalSection(&s_Lock);
	if (!s_Queue.empty())
	{
		u32 index = 0;
		if (progress_only)
		{
			for (; index < s_Queue.size(); ++index)
				if (s_Queue[index]->cmd == "progress")
					break;
		}
		if (index < s_Queue.size())
		{
			request = s_Queue[index];
			s_Queue.erase(s_Queue.begin() + index);
		}
		requests_remain = !s_Queue.empty();
	}
	::LeaveCriticalSection(&s_Lock);
	if (requests_remain && s_QueueEvent)
		::SetEvent(s_QueueEvent);
	return request;
}

static void ExecuteQueuedRequest(SMCPRequest* request)
{
	if (!request)
		return;
	if (!IsRunning())
	{
		request->Cancel();
		request->Release();
		return;
	}
	if (SMCPRequest::Queued == ::InterlockedCompareExchange(
		&request->state, SMCPRequest::Executing, SMCPRequest::Queued))
	{
		Execute(*request);
		::InterlockedExchange(&request->state, SMCPRequest::Completed);
		::SetEvent(request->done);
	}
	request->Release();
}

void XFinedMCP::Pump()
{
	ExecuteQueuedRequest(TakeQueuedRequest(false));
}

void XFinedMCP::PumpProgressRequests()
{
	ExecuteQueuedRequest(TakeQueuedRequest(true));
}

//------------------------------------------------------------------------------
// socket thread
//------------------------------------------------------------------------------
enum EReceiveResult
{
	ReceiveOpen,
	ReceiveClosed,
	ReceiveOverflow
};

enum ERequestWaitResult
{
	RequestReady,
	RequestTimedOut,
	RequestStopped,
	RequestDisconnected,
	RequestOverflow
};

static bool SendAll(SOCKET client, const char* data, size_t size)
{
	size_t sent = 0;
	while (sent < size)
	{
		const int chunk = int(_min(size - sent, size_t(INT_MAX)));
		const int n = ::send(client, data + sent, chunk, 0);
		if (n > 0)
		{
			sent += size_t(n);
			continue;
		}

		const int error = ::WSAGetLastError();
		if (SOCKET_ERROR != n || WSAEWOULDBLOCK != error)
			return false;

		fd_set writable;
		FD_ZERO(&writable);
		FD_SET(client, &writable);
		timeval wait = { 0, 100000 };
		if (::select(0, NULL, &writable, NULL, &wait) == SOCKET_ERROR)
			return false;
		if (!IsRunning() || (s_StopEvent && WAIT_OBJECT_0 == ::WaitForSingleObject(s_StopEvent, 0)))
			return false;
	}
	return true;
}

static EReceiveResult ReceiveAvailable(SOCKET client, xr_string& accumulator)
{
	char buffer[4096];
	for (;;)
	{
		const int n = ::recv(client, buffer, sizeof(buffer), 0);
		if (n > 0)
		{
			if (accumulator.size() + size_t(n) > kMaxRequestBytes)
				return ReceiveOverflow;
			accumulator.append(buffer, n);
			continue;
		}
		if (!n)
			return ReceiveClosed;

		const int error = ::WSAGetLastError();
		if (WSAEWOULDBLOCK == error)
			return ReceiveOpen;
		if (WSAEINTR != error)
			return ReceiveClosed;
	}
}

static EReceiveResult PollClient(SOCKET client, xr_string& accumulator)
{
	fd_set readable;
	FD_ZERO(&readable);
	FD_SET(client, &readable);
	timeval now = { 0, 0 };
	const int selected = ::select(0, &readable, NULL, NULL, &now);
	if (selected == SOCKET_ERROR)
		return ReceiveClosed;
	return selected > 0 ? ReceiveAvailable(client, accumulator) : ReceiveOpen;
}

static bool QueueRequest(SMCPRequest* request)
{
	bool queued = false;
	::EnterCriticalSection(&s_Lock);
	if (IsRunning() && s_Queue.size() < kMaxQueuedRequests)
	{
		s_Queue.push_back(request);
		request->AddRef();
		queued = true;
	}
	::LeaveCriticalSection(&s_Lock);
	if (queued && s_QueueEvent)
		::SetEvent(s_QueueEvent);
	return queued;
}

static ERequestWaitResult WaitForRequest(SOCKET client, SMCPRequest* request, xr_string& accumulator)
{
	const ULONGLONG deadline = ::GetTickCount64() + s_RequestTimeoutMs;
	HANDLE events[] = { request->done, s_StopEvent };
	for (;;)
	{
		const ULONGLONG now = ::GetTickCount64();
		if (now >= deadline)
			return RequestTimedOut;

		const DWORD remaining = DWORD(_min(deadline - now, ULONGLONG(25)));
		const DWORD result = ::WaitForMultipleObjects(_countof(events), events, FALSE, remaining);
		if (WAIT_OBJECT_0 == result)
			return RequestReady;
		if (WAIT_OBJECT_0 + 1 == result || !IsRunning())
			return RequestStopped;
		if (WAIT_TIMEOUT != result)
			return RequestStopped;

		switch (PollClient(client, accumulator))
		{
		case ReceiveClosed:		return RequestDisconnected;
		case ReceiveOverflow:	return RequestOverflow;
		default:				break;
		}
	}
}

static bool ProcessLine(SOCKET client, const xr_string& line, xr_string& accumulator)
{
	char command[128];
	if (!XFinedMCP::GetArg(line.c_str(), "cmd", command, sizeof(command)) || !command[0])
	{
		const xr_string out = "{\"ok\":false,\"error\":\"invalid JSON request or missing string 'cmd'\"}\n";
		return SendAll(client, out.c_str(), out.size());
	}

	SMCPRequest* request = xr_new<SMCPRequest>();
	if (!request->done)
	{
		request->Release();
		const xr_string out = "{\"ok\":false,\"error\":\"request event allocation failed\"}\n";
		return SendAll(client, out.c_str(), out.size());
	}

	request->cmd = command;
	request->raw = line;
	if (!QueueRequest(request))
	{
		request->Release();
		const xr_string out = IsRunning()
			? "{\"ok\":false,\"error\":\"MCP queue full\"}\n"
			: "{\"ok\":false,\"error\":\"editor shutting down\"}\n";
		return SendAll(client, out.c_str(), out.size());
	}

	xr_string out;
	const ERequestWaitResult result = WaitForRequest(client, request, accumulator);
	if (RequestReady == result)
		out = request->response;
	else
		request->Cancel();

	if (RequestTimedOut == result)
		out = "{\"ok\":false,\"error\":\"editor busy (timeout)\"}";
	else if (RequestOverflow == result)
		out = "{\"ok\":false,\"error\":\"request buffer exceeds 1 MiB\"}";

	request->Release();
	if (RequestStopped == result || RequestDisconnected == result)
		return false;

	out += "\n";
	return SendAll(client, out.c_str(), out.size()) && RequestOverflow != result;
}

static void ServeClient(SOCKET client)
{
	xr_string accumulator;
	while (IsRunning())
	{
		const size_t newline = accumulator.find('\n');
		if (newline != xr_string::npos)
		{
			xr_string line = accumulator.substr(0, newline);
			accumulator.erase(0, newline + 1);
			if (!line.empty() && '\r' == line.back())
				line.pop_back();
			if (!ProcessLine(client, line, accumulator))
				break;
			continue;
		}

		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(client, &readable);
		timeval wait = { 0, 100000 };
		const int selected = ::select(0, &readable, NULL, NULL, &wait);
		if (selected == SOCKET_ERROR)
			break;
		if (selected > 0)
		{
			const EReceiveResult result = ReceiveAvailable(client, accumulator);
			if (ReceiveOverflow == result)
			{
				const xr_string out = "{\"ok\":false,\"error\":\"request buffer exceeds 1 MiB\"}\n";
				SendAll(client, out.c_str(), out.size());
			}
			if (ReceiveOpen != result)
				break;
		}
	}
}

static DWORD WINAPI ClientThread(LPVOID parameter)
{
	SMCPClient* client = static_cast<SMCPClient*>(parameter);
	ServeClient(client->socket);

	::EnterCriticalSection(&s_Lock);
	const SOCKET socket = client->socket;
	client->socket = INVALID_SOCKET;
	if (socket != INVALID_SOCKET)
		::closesocket(socket);
	::LeaveCriticalSection(&s_Lock);
	return 0;
}

static void ReapFinishedClients()
{
	for (u32 i = 0; i < s_Clients.size();)
	{
		SMCPClient* client = s_Clients[i];
		if (!client->thread || WAIT_OBJECT_0 != ::WaitForSingleObject(client->thread, 0))
		{
			++i;
			continue;
		}

		::CloseHandle(client->thread);
		xr_delete(client);
		s_Clients.erase(s_Clients.begin() + i);
	}
}

static DWORD WINAPI AcceptThread(LPVOID)
{
	while (IsRunning())
	{
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(s_Listen, &readable);
		timeval wait = { 0, 100000 };
		const int selected = ::select(0, &readable, NULL, NULL, &wait);
		if (selected == SOCKET_ERROR)
			break;
		if (!selected)
			continue;

		SOCKET socket = ::accept(s_Listen, NULL, NULL);
		if (socket == INVALID_SOCKET)
		{
			if (WSAEWOULDBLOCK == ::WSAGetLastError())
				continue;
			break;
		}
		u_long non_blocking = 1;
		if (::ioctlsocket(socket, FIONBIO, &non_blocking) == SOCKET_ERROR)
		{
			::closesocket(socket);
			continue;
		}

		ReapFinishedClients();
		if (!IsRunning())
		{
			::closesocket(socket);
			break;
		}
		if (s_Clients.size() >= kMaxClients)
		{
			const xr_string out = "{\"ok\":false,\"error\":\"too many MCP clients\"}\n";
			SendAll(socket, out.c_str(), out.size());
			::closesocket(socket);
			continue;
		}

		SMCPClient* client = xr_new<SMCPClient>(socket);
		client->thread = ::CreateThread(NULL, 0, ClientThread, client, 0, NULL);
		if (!client->thread)
		{
			::closesocket(socket);
			xr_delete(client);
			continue;
		}
		s_Clients.push_back(client);
	}
	return 0;
}

static void CleanupStartupFailure()
{
	::InterlockedExchange(&s_Run, 0);
	if (s_Listen != INVALID_SOCKET)
	{
		::closesocket(s_Listen);
		s_Listen = INVALID_SOCKET;
	}
	if (s_StopEvent)
	{
		::CloseHandle(s_StopEvent);
		s_StopEvent = 0;
	}
	if (s_QueueEvent)
	{
		::CloseHandle(s_QueueEvent);
		s_QueueEvent = 0;
	}
	if (s_WsaReady)
	{
		::WSACleanup();
		s_WsaReady = false;
	}
	if (s_LockReady)
	{
		::DeleteCriticalSection(&s_Lock);
		s_LockReady = false;
	}
}

void XFinedMCP::Start()
{
	if (s_LockReady)
		return;

	::InitializeCriticalSection(&s_Lock);
	s_LockReady = true;
	s_RequestTimeoutMs = kDefaultRequestTimeoutMs;
	char timeout[32] = {};
	const DWORD timeout_size = ::GetEnvironmentVariableA("XFINED_MCP_REQUEST_TIMEOUT_MS", timeout, sizeof(timeout));
	if (timeout_size && timeout_size < sizeof(timeout))
	{
		char* end = 0;
		const unsigned long value = std::strtoul(timeout, &end, 10);
		if (end && !*end && value && value <= 3600000)
			s_RequestTimeoutMs = DWORD(value);
	}

	WSADATA wsa;
	if (::WSAStartup(MAKEWORD(2, 2), &wsa))
	{
		CleanupStartupFailure();
		return;
	}
	s_WsaReady = true;
	s_StopEvent = ::CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!s_StopEvent)
	{
		CleanupStartupFailure();
		return;
	}
	// Auto-reset is intentional. Pump re-signals while requests remain, so the
	// frame loop handles one command at a time without a queue-check-to-wait race.
	s_QueueEvent = ::CreateEventA(NULL, FALSE, FALSE, NULL);
	if (!s_QueueEvent)
	{
		CleanupStartupFailure();
		return;
	}

	s_Listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s_Listen == INVALID_SOCKET)
	{
		CleanupStartupFailure();
		return;
	}

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kPort);
	addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
	if (::bind(s_Listen, (sockaddr*)&addr, sizeof(addr)) || ::listen(s_Listen, SOMAXCONN))
	{
		Msg("! [MCP] port %d busy — endpoint disabled", kPort);
		CleanupStartupFailure();
		return;
	}
	u_long non_blocking = 1;
	if (::ioctlsocket(s_Listen, FIONBIO, &non_blocking) == SOCKET_ERROR)
	{
		CleanupStartupFailure();
		return;
	}
	::InterlockedExchange(&s_Run, 1);
	s_AcceptThread = ::CreateThread(NULL, 0, AcceptThread, NULL, 0, NULL);
	if (!s_AcceptThread)
	{
		CleanupStartupFailure();
		return;
	}
	Msg("* [MCP] listening on 127.0.0.1:%d", kPort);
}

void XFinedMCP::Stop()
{
	if (!s_LockReady)
		return;

	::InterlockedExchange(&s_Run, 0);
	if (s_StopEvent)
		::SetEvent(s_StopEvent);
	if (s_Listen != INVALID_SOCKET)
	{
		::shutdown(s_Listen, kShutdownBoth);
		::closesocket(s_Listen);
		s_Listen = INVALID_SOCKET;
	}
	if (s_AcceptThread)
	{
		::WaitForSingleObject(s_AcceptThread, INFINITE);
		::CloseHandle(s_AcceptThread);
		s_AcceptThread = 0;
	}

	::EnterCriticalSection(&s_Lock);
	for (u32 i = 0; i < s_Clients.size(); ++i)
		if (s_Clients[i]->socket != INVALID_SOCKET)
			::shutdown(s_Clients[i]->socket, kShutdownBoth);
	::LeaveCriticalSection(&s_Lock);

	for (u32 i = 0; i < s_Clients.size(); ++i)
	{
		if (s_Clients[i]->thread)
		{
			::WaitForSingleObject(s_Clients[i]->thread, INFINITE);
			::CloseHandle(s_Clients[i]->thread);
		}
		xr_delete(s_Clients[i]);
	}
	s_Clients.clear();

	xr_vector<SMCPRequest*> abandoned;
	::EnterCriticalSection(&s_Lock);
	abandoned.swap(s_Queue);
	::LeaveCriticalSection(&s_Lock);
	for (u32 i = 0; i < abandoned.size(); ++i)
	{
		abandoned[i]->Cancel();
		abandoned[i]->Release();
	}

	if (s_StopEvent)
	{
		::CloseHandle(s_StopEvent);
		s_StopEvent = 0;
	}
	if (s_QueueEvent)
	{
		::CloseHandle(s_QueueEvent);
		s_QueueEvent = 0;
	}
	if (s_WsaReady)
	{
		::WSACleanup();
		s_WsaReady = false;
	}
	::DeleteCriticalSection(&s_Lock);
	s_LockReady = false;
}

HANDLE XFinedMCP::WakeEvent()
{
	return s_QueueEvent;
}
