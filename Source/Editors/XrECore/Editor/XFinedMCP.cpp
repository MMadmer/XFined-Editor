#include "stdafx.h"
#include "XFinedMCP.h"
#include "EditorProject.h"
#include "UI_ToolsCustom.h"
#include "ui_main.h"
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

//------------------------------------------------------------------------------
// request queue: socket thread produces, main thread consumes
//------------------------------------------------------------------------------
struct SMCPRequest
{
	xr_string		cmd;
	xr_string		raw;		// full request line, for handler arguments
	xr_string		response;
	HANDLE			done;
};

static TXFinedMCPHandler	s_Handler = 0;
void XFinedMCP::SetHandler(TXFinedMCPHandler handler) { s_Handler = handler; }

bool XFinedMCP::GetArg(LPCSTR raw, LPCSTR field, char* dst, u32 dst_size)
{
	dst[0] = 0;
	if (!raw) return false;
	char pat[64];
	sprintf_s(pat, "\"%s\"", field);
	const char* k = strstr(raw, pat);
	if (!k) return false;
	const char* c = strchr(k + strlen(pat), ':');
	if (!c) return false;
	const char* q1 = strchr(c, '"');
	if (!q1) return false;

	// The value is a JSON string, so an escaped quote does NOT end it and the
	// text has to be unescaped on the way out: callers want what the sender
	// meant, not the wire form. Scanning for the closing quote with strchr got
	// both wrong - a search term like "two words" came back as a lone
	// backslash, and every path arrived with its separators still doubled.
	u32 w = 0;
	for (const char* r = q1 + 1; *r && w + 1 < dst_size; ++r)
	{
		if (*r == '"') break;
		char ch = *r;
		if (ch == '\\' && r[1])
		{
			switch (*++r)
			{
			case 'n':	ch = '\n';	break;
			case 't':	ch = '\t';	break;
			case 'r':	ch = '\r';	break;
			case 'b':	ch = '\b';	break;
			case 'f':	ch = '\f';	break;
			case 'u':
				{
					// \uXXXX -> UTF-8. Not every argument is a path or an id any
					// more: a game mode's title is text the player reads, and
					// folding it to '?' would ship a mod whose campaign has no
					// name. The sender escapes everything above ascii (json.dumps
					// defaults to ensure_ascii), so this is the ONLY way non-latin
					// text arrives.
					int v = 0, got = 0;
					for (; got < 4; ++got)
					{
						const char d = r[1];
						const int  h = (d >= '0' && d <= '9') ? d - '0'
									 : (d >= 'a' && d <= 'f') ? d - 'a' + 10
									 : (d >= 'A' && d <= 'F') ? d - 'A' + 10 : -1;
						if (h < 0) break;
						v = v * 16 + h;
						++r;
					}
					if (4 != got) { ch = '?'; break; }

					// a high surrogate takes its partner with it
					if (v >= 0xD800 && v <= 0xDBFF && r[1] == '\\' && r[2] == 'u')
					{
						const char* keep = r;
						int lo = 0, got2 = 0;
						r += 2;
						for (; got2 < 4; ++got2)
						{
							const char d = r[1];
							const int  h = (d >= '0' && d <= '9') ? d - '0'
										 : (d >= 'a' && d <= 'f') ? d - 'a' + 10
										 : (d >= 'A' && d <= 'F') ? d - 'A' + 10 : -1;
							if (h < 0) break;
							lo = lo * 16 + h;
							++r;
						}
						if (4 == got2 && lo >= 0xDC00 && lo <= 0xDFFF)
							v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
						else
							r = keep;
					}

					char utf[4];
					int n = 0;
					if (v < 0x80)			{ utf[n++] = char(v); }
					else if (v < 0x800)		{ utf[n++] = char(0xC0 | (v >> 6));   utf[n++] = char(0x80 | (v & 0x3F)); }
					else if (v < 0x10000)	{ utf[n++] = char(0xE0 | (v >> 12));  utf[n++] = char(0x80 | ((v >> 6) & 0x3F));
											  utf[n++] = char(0x80 | (v & 0x3F)); }
					else					{ utf[n++] = char(0xF0 | (v >> 18));  utf[n++] = char(0x80 | ((v >> 12) & 0x3F));
											  utf[n++] = char(0x80 | ((v >> 6) & 0x3F)); utf[n++] = char(0x80 | (v & 0x3F)); }
					for (int i = 0; i < n && w + 1 < dst_size; ++i)
						dst[w++] = utf[i];
					continue;
				}
			default:	ch = *r;	break;		// \" \\ \/ and anything unknown
			}
		}
		dst[w++] = ch;
	}
	dst[w] = 0;
	return true;
}

static CRITICAL_SECTION		s_Lock;
static xr_vector<SMCPRequest*>	s_Queue;
static SOCKET				s_Listen	= INVALID_SOCKET;
static HANDLE				s_Thread	= 0;
static volatile bool		s_Run		= false;

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
				if (EditorProject::Active()) EditorProject::Close();
				if (EditorProject::Open(path))
					r.response = "{\"ok\":true}";
				else
					r.response = "{\"ok\":false,\"error\":\"can't open project (missing folder?)\"}";
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

void XFinedMCP::Pump()
{
	::EnterCriticalSection(&s_Lock);
	xr_vector<SMCPRequest*> batch = s_Queue;
	s_Queue.clear();
	::LeaveCriticalSection(&s_Lock);
	for (u32 i = 0; i < batch.size(); ++i)
	{
		Execute(*batch[i]);
		::SetEvent(batch[i]->done);
	}
}

//------------------------------------------------------------------------------
// socket thread
//------------------------------------------------------------------------------
static void ServeClient(SOCKET client)
{
	xr_string acc;
	char buf[4096];
	for (;;)
	{
		const int n = ::recv(client, buf, sizeof(buf), 0);
		if (n <= 0) break;
		acc.append(buf, n);
		size_t nl;
		while ((nl = acc.find('\n')) != xr_string::npos)
		{
			xr_string line = acc.substr(0, nl);
			acc.erase(0, nl + 1);
			// micro-parse: {"cmd":"..."} — the only field we care about
			xr_string cmd;
			const size_t k = line.find("\"cmd\"");
			if (k != xr_string::npos)
			{
				const size_t q1 = line.find('"', line.find(':', k));
				const size_t q2 = (q1 != xr_string::npos) ? line.find('"', q1 + 1) : xr_string::npos;
				if (q2 != xr_string::npos) cmd = line.substr(q1 + 1, q2 - q1 - 1);
			}

			SMCPRequest req;
			req.cmd = cmd;
			req.raw = line;
			req.done = ::CreateEventA(NULL, FALSE, FALSE, NULL);
			::EnterCriticalSection(&s_Lock);
			s_Queue.push_back(&req);
			::LeaveCriticalSection(&s_Lock);

			xr_string out;
			// generous: scene loads run synchronously on the main thread
			if (::WaitForSingleObject(req.done, 180000) == WAIT_OBJECT_0)
				out = req.response;
			else
				out = "{\"ok\":false,\"error\":\"editor busy (timeout)\"}";
			::CloseHandle(req.done);

			out += "\n";
			::send(client, out.c_str(), (int)out.size(), 0);
		}
	}
	::closesocket(client);
}

static DWORD WINAPI AcceptThread(LPVOID)
{
	while (s_Run)
	{
		SOCKET client = ::accept(s_Listen, NULL, NULL);
		if (client == INVALID_SOCKET) break;
		ServeClient(client);
	}
	return 0;
}

void XFinedMCP::Start()
{
	if (s_Thread) return;
	::InitializeCriticalSection(&s_Lock);
	WSADATA wsa;
	if (::WSAStartup(MAKEWORD(2, 2), &wsa)) return;

	s_Listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s_Listen == INVALID_SOCKET) return;

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kPort);
	addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
	if (::bind(s_Listen, (sockaddr*)&addr, sizeof(addr)) || ::listen(s_Listen, 1))
	{
		Msg("! [MCP] port %d busy — endpoint disabled", kPort);
		::closesocket(s_Listen);
		s_Listen = INVALID_SOCKET;
		return;
	}
	s_Run = true;
	s_Thread = ::CreateThread(NULL, 0, AcceptThread, NULL, 0, NULL);
	Msg("* [MCP] listening on 127.0.0.1:%d", kPort);
}

void XFinedMCP::Stop()
{
	if (!s_Thread) return;
	s_Run = false;
	if (s_Listen != INVALID_SOCKET) ::closesocket(s_Listen);
	::WaitForSingleObject(s_Thread, 2000);
	::CloseHandle(s_Thread);
	s_Thread = 0;
	::DeleteCriticalSection(&s_Lock);
}
