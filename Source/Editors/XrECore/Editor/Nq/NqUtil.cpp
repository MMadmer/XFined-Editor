#include "stdafx.h"
#include "NqUtil.h"
#include "../EditorProject.h"

//==============================================================================
// encoding
//==============================================================================
xr_string NqUtil::Cp1251ToUtf8(LPCSTR src)
{
	xr_string out;
	if (!src || !src[0]) return out;
	const int wide_len = ::MultiByteToWideChar(1251, 0, src, -1, NULL, 0);
	if (wide_len <= 0) { out = src; return out; }
	xr_vector<wchar_t> wide(wide_len);
	::MultiByteToWideChar(1251, 0, src, -1, wide.data(), wide_len);
	const int utf_len = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, NULL, 0, NULL, NULL);
	if (utf_len <= 0) { out = src; return out; }
	xr_vector<char> utf(utf_len);
	::WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, utf.data(), utf_len, NULL, NULL);
	out = utf.data();
	return out;
}

xr_string NqUtil::Utf8ToCp1251(LPCSTR src)
{
	xr_string out;
	if (!src || !src[0]) return out;
	const int wide_len = ::MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
	if (wide_len <= 0) { out = src; return out; }
	xr_vector<wchar_t> wide(wide_len);
	::MultiByteToWideChar(CP_UTF8, 0, src, -1, wide.data(), wide_len);
	const int ansi_len = ::WideCharToMultiByte(1251, 0, wide.data(), -1, NULL, 0, NULL, NULL);
	if (ansi_len <= 0) { out = src; return out; }
	xr_vector<char> ansi(ansi_len);
	::WideCharToMultiByte(1251, 0, wide.data(), -1, ansi.data(), ansi_len, "?", NULL);
	out = ansi.data();
	return out;
}

bool NqUtil::Cp1251Covers(const xr_string& utf8, xr_string& bad)
{
	bad.clear();
	if (utf8.empty()) return true;
	const int wide_len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
	if (wide_len <= 0) return true;
	xr_vector<wchar_t> wide(wide_len);
	::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), wide_len);
	xr_vector<u32> seen;
	for (int i = 0; i < wide_len; ++i)
	{
		const wchar_t c = wide[i];
		if (c < 0x80) continue;
		// a supplementary character (emoji) is a surrogate pair - two units
		int units = 1;
		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < wide_len && wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) units = 2;
		char a[8]; BOOL used = FALSE;
		// WC_NO_BEST_FIT_CHARS: a character that only maps through a look-alike
		// is not covered - the game would show something else
		const int n = ::WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, &wide[i], units, a, sizeof(a), "?", &used);
		const u32 cp = (units == 2) ? 0x10000 + (((u32)c - 0xD800) << 10) + ((u32)wide[i + 1] - 0xDC00) : (u32)c;
		if (units == 2) ++i;
		if (n > 0 && !used) continue;
		bool dup = false;
		for (u32 k = 0; k < seen.size(); ++k) if (seen[k] == cp) dup = true;
		if (dup) continue;
		seen.push_back(cp);
		if (seen.size() > 6) break;
		char u[8] = {};
		::WideCharToMultiByte(CP_UTF8, 0, &wide[i - (units - 1)], units, u, sizeof(u), NULL, NULL);
		if (!bad.empty()) bad += " ";
		bad += u;
	}
	return seen.empty();
}

xr_string NqUtil::GameTextToUtf8(const char* bytes, u32 size)
{
	xr_string s;
	if (!bytes || !size) return s;
	if (size >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
	{
		s.assign(bytes + 3, bytes + size);
		return s;
	}
	xr_string raw; raw.assign(bytes, bytes + size);
	// valid UTF-8 with multibyte sequences passes; anything else is cp1251
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.c_str(), (int)raw.size(), NULL, 0) > 0)
	{
		bool high = false;
		for (u32 i = 0; i < raw.size() && !high; ++i) high = (unsigned char)raw[i] >= 0x80;
		if (high) return raw;
	}
	return Cp1251ToUtf8(raw.c_str());
}

xr_string NqUtil::ClipUtf8(const xr_string& s, u32 max_bytes)
{
	if (s.size() <= max_bytes) return s;
	u32 cut = max_bytes;
	// back off to the start of a code point (continuation bytes are 10xxxxxx)
	while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
	xr_string r = s.substr(0, cut);
	r += "...";
	return r;
}

//==============================================================================
// json
//==============================================================================
void NqUtil::JsonAppend(xr_string& out, LPCSTR s)
{
	for (const char* p = s ? s : ""; *p; ++p)
	{
		const unsigned char c = (unsigned char)*p;
		switch (c)
		{
		case '"':	out += "\\\"";	break;
		case '\\':	out += "\\\\";	break;
		case '\n':	out += "\\n";	break;
		case '\r':	out += "\\r";	break;
		case '\t':	out += "\\t";	break;
		default:
			if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", (int)c); out += b; }
			else out += (char)c;
		}
	}
}

void NqUtil::JsonAppend(xr_string& out, const xr_string& s)		{ JsonAppend(out, s.c_str()); }
void NqUtil::JsonString(xr_string& out, LPCSTR s)				{ out += '"'; JsonAppend(out, s); out += '"'; }
void NqUtil::JsonString(xr_string& out, const xr_string& s)		{ JsonString(out, s.c_str()); }

void NqUtil::JsonPath(xr_string& out, LPCSTR s)
{
	xr_string p = s ? s : "";
	for (u32 i = 0; i < p.size(); ++i) if (p[i] == '\\') p[i] = '/';
	JsonString(out, p);
}

void NqUtil::JsonNumber(xr_string& out, double n)
{
	char b[64];
	if (n == (double)(long long)n && fabs(n) < 9.0e15) sprintf_s(b, "%lld", (long long)n);
	else sprintf_s(b, "%.15g", n);
	out += b;
}

void NqUtil::JsonInt(xr_string& out, int n)		{ char b[32]; sprintf_s(b, "%d", n); out += b; }
void NqUtil::JsonBool(xr_string& out, bool b)	{ out += b ? "true" : "false"; }

void NqUtil::JsonStringArray(xr_string& out, const xr_vector<xr_string>& v)
{
	out += "[";
	for (u32 i = 0; i < v.size(); ++i) { if (i) out += ","; JsonString(out, v[i]); }
	out += "]";
}

void NqUtil::JsonError(xr_string& out, LPCSTR message)
{
	out = "{\"ok\":false,\"error\":";
	JsonString(out, message);
	out += "}";
}

void NqUtil::JsonError(xr_string& out, const xr_string& message)	{ JsonError(out, message.c_str()); }

//==============================================================================
// request arguments
//==============================================================================
static const char* FindField(LPCSTR raw, LPCSTR field)
{
	if (!raw || !field) return 0;
	char pat[96];
	sprintf_s(pat, "\"%s\"", field);
	const size_t plen = strlen(pat);
	// a key is a quoted name followed by ':' - a same-named value ("path":"id")
	// must not match, and a longer key that ends with the name must not either
	for (const char* k = strstr(raw, pat); k; k = strstr(k + 1, pat))
	{
		const char* c = k + plen;
		while (*c == ' ' || *c == '\t') ++c;
		if (*c != ':') continue;
		return c + 1;
	}
	return 0;
}

bool NqUtil::ArgBool(LPCSTR raw, LPCSTR field, bool def)
{
	const char* c = FindField(raw, field);
	if (!c) return def;
	while (*c == ' ' || *c == '"') ++c;
	if (0 == strncmp(c, "true", 4) || *c == '1') return true;
	if (0 == strncmp(c, "false", 5) || *c == '0') return false;
	return def;
}

int NqUtil::ArgInt(LPCSTR raw, LPCSTR field, int def)
{
	const char* c = FindField(raw, field);
	if (!c) return def;
	while (*c == ' ' || *c == '\t' || *c == '"') ++c;
	if (*c != '-' && (*c < '0' || *c > '9')) return def;
	return atoi(c);
}

bool NqUtil::ArgFloat(LPCSTR raw, LPCSTR field, float& out)
{
	const char* c = FindField(raw, field);
	if (!c) return false;
	while (*c == ' ' || *c == '\t' || *c == '"') ++c;
	if (*c != '-' && *c != '.' && (*c < '0' || *c > '9')) return false;
	out = (float)atof(c);
	return true;
}

bool NqUtil::ArgString(LPCSTR raw, LPCSTR field, xr_string& out)
{
	out.clear();
	const char* c = FindField(raw, field);
	if (!c) return false;
	while (*c == ' ' || *c == '\t') ++c;
	if (*c != '"')
	{
		// bare value (number/bool) - take its text
		const char* e = c;
		while (*e && *e != ',' && *e != '}' && *e != ' ') ++e;
		out.assign(c, e);
		return true;
	}
	// same unescaping as XFinedMCP::GetArg, without the fixed buffer
	for (const char* r = c + 1; *r; ++r)
	{
		if (*r == '"') break;
		char ch = *r;
		if (ch == '\\' && r[1])
		{
			switch (*++r)
			{
			case 'n': ch = '\n'; break;
			case 't': ch = '\t'; break;
			case 'r': ch = '\r'; break;
			case 'b': ch = '\b'; break;
			case 'f': ch = '\f'; break;
			case 'u':
				{
					int v = 0, got = 0;
					for (; got < 4; ++got)
					{
						const char d = r[1];
						const int h = (d >= '0' && d <= '9') ? d - '0' : (d >= 'a' && d <= 'f') ? d - 'a' + 10
								   : (d >= 'A' && d <= 'F') ? d - 'A' + 10 : -1;
						if (h < 0) break;
						v = v * 16 + h; ++r;
					}
					if (4 != got) { ch = '?'; break; }
					if (v >= 0xD800 && v <= 0xDBFF && r[1] == '\\' && r[2] == 'u')
					{
						const char* keep = r;
						int lo = 0, got2 = 0;
						r += 2;
						for (; got2 < 4; ++got2)
						{
							const char d = r[1];
							const int h = (d >= '0' && d <= '9') ? d - '0' : (d >= 'a' && d <= 'f') ? d - 'a' + 10
									   : (d >= 'A' && d <= 'F') ? d - 'A' + 10 : -1;
							if (h < 0) break;
							lo = lo * 16 + h; ++r;
						}
						if (4 == got2 && lo >= 0xDC00 && lo <= 0xDFFF) v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
						else r = keep;
					}
					if (v < 0x80)			{ out += char(v); }
					else if (v < 0x800)		{ out += char(0xC0 | (v >> 6)); out += char(0x80 | (v & 0x3F)); }
					else if (v < 0x10000)	{ out += char(0xE0 | (v >> 12)); out += char(0x80 | ((v >> 6) & 0x3F)); out += char(0x80 | (v & 0x3F)); }
					else					{ out += char(0xF0 | (v >> 18)); out += char(0x80 | ((v >> 12) & 0x3F));
											  out += char(0x80 | ((v >> 6) & 0x3F)); out += char(0x80 | (v & 0x3F)); }
					continue;
				}
			default: ch = *r; break;
			}
		}
		out += ch;
	}
	return true;
}

//==============================================================================
// paths and files
//==============================================================================
static string_path s_SdkRoot = {};
static string_path s_AppData = {};

LPCSTR NqUtil::SdkRoot()
{
	if (s_SdkRoot[0]) return s_SdkRoot;
	// the exe lives in Bin\x64\<cfg>\; the root is the folder holding fs.ltx
	::GetModuleFileNameA(NULL, s_SdkRoot, sizeof(s_SdkRoot));
	if (char* p = strrchr(s_SdkRoot, '\\')) *p = 0;
	for (int up = 0; up < 6; ++up)
	{
		string_path probe;
		sprintf_s(probe, "%s\\fs.ltx", s_SdkRoot);
		if (INVALID_FILE_ATTRIBUTES != ::GetFileAttributesA(probe)) break;
		char* cut = strrchr(s_SdkRoot, '\\');
		if (!cut) break;
		*cut = 0;
	}
	return s_SdkRoot;
}

LPCSTR NqUtil::AppDataRoot()
{
	if (s_AppData[0]) return s_AppData;
	sprintf_s(s_AppData, "%s\\_appdata_", SdkRoot());
	::CreateDirectoryA(s_AppData, NULL);
	return s_AppData;
}

bool NqUtil::FileExists(LPCSTR path)
{
	const DWORD a = path ? ::GetFileAttributesA(path) : INVALID_FILE_ATTRIBUTES;
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool NqUtil::DirExists(LPCSTR path)
{
	const DWORD a = path ? ::GetFileAttributesA(path) : INVALID_FILE_ATTRIBUTES;
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void NqUtil::NormalizePath(LPCSTR in, xr_string& out)
{
	out.clear();
	if (!in) return;
	for (const char* p = in; *p; ++p)
	{
		char c = (*p == '/') ? '\\' : *p;
		if (c == '\\' && !out.empty() && out.back() == '\\') continue;	// "a\\b"
		out += c;
	}
	// "\.\" segments
	for (;;)
	{
		const size_t at = out.find("\\.\\");
		if (at == xr_string::npos) break;
		out.erase(at, 2);
	}
	while (out.size() > 3 && out.back() == '\\') out.pop_back();
}

xr_string NqUtil::PathKey(LPCSTR path)
{
	xr_string n;
	NormalizePath(path, n);
	for (u32 i = 0; i < n.size(); ++i) n[i] = (char)tolower((unsigned char)n[i]);
	return n;
}

bool NqUtil::ProjectPath(LPCSTR rel_or_abs, xr_string& out, xr_string& err)
{
	out.clear(); err.clear();
	if (!rel_or_abs || !rel_or_abs[0]) { err = "path is empty"; return false; }
	xr_string n;
	NormalizePath(rel_or_abs, n);
	const bool absolute = (n.size() >= 2 && n[1] == ':') || (n.size() >= 2 && n[0] == '\\' && n[1] == '\\');
	if (absolute) { out = n; return true; }
	if (!EditorProject::Active()) { err = "no active project"; return false; }
	xr_string root;
	NormalizePath(EditorProject::Root(), root);
	// "..": a project-relative path never escapes the project
	if (n.find("..") != xr_string::npos) { err = "path may not contain '..'"; return false; }
	out = root;
	out += "\\";
	out += n;
	return true;
}

xr_string NqUtil::ProjectRelative(LPCSTR abs)
{
	xr_string n;
	NormalizePath(abs, n);
	if (EditorProject::Active())
	{
		xr_string root;
		NormalizePath(EditorProject::Root(), root);
		if (n.size() > root.size() + 1 && 0 == _strnicmp(n.c_str(), root.c_str(), root.size()) && n[root.size()] == '\\')
			n = n.substr(root.size() + 1);
	}
	for (u32 i = 0; i < n.size(); ++i) if (n[i] == '\\') n[i] = '/';
	return n;
}

static void ListFilesRec(LPCSTR dir, LPCSTR ext, xr_vector<xr_string>& out, NqUtil::TSkipRootEntry skip, bool top)
{
	string_path mask;
	sprintf_s(mask, "%s\\*", dir);
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA(mask, &fd);
	if (INVALID_HANDLE_VALUE == h) return;
	do
	{
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		if (top && skip && skip(fd.cFileName)) continue;
		string_path full;
		sprintf_s(full, "%s\\%s", dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ListFilesRec(full, ext, out, skip, false); continue; }
		const size_t nl = strlen(fd.cFileName), el = strlen(ext);
		if (nl > el && 0 == _stricmp(fd.cFileName + nl - el, ext)) out.push_back(xr_string(full));
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
}

void NqUtil::ListFiles(LPCSTR root, LPCSTR ext, xr_vector<xr_string>& out, TSkipRootEntry skip)
{
	out.clear();
	if (!root || !root[0] || !ext) return;
	ListFilesRec(root, ext, out, skip, true);
	std::sort(out.begin(), out.end());
}

xr_string NqUtil::BaseName(LPCSTR path)
{
	if (!path) return xr_string();
	const char* base = path;
	for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
	xr_string r = base;
	const size_t dot = r.rfind('.');
	if (dot != xr_string::npos && dot > 0) r.erase(dot);
	return r;
}

xr_string NqUtil::Extension(LPCSTR path)
{
	if (!path) return xr_string();
	const char* base = path;
	for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
	const char* dot = strrchr(base, '.');
	xr_string r = dot ? dot : "";
	for (u32 i = 0; i < r.size(); ++i) r[i] = (char)tolower((unsigned char)r[i]);
	return r;
}

//==============================================================================
// misc
//==============================================================================
xr_string NqUtil::Trim(const xr_string& s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
	return s.substr(b, e - b);
}

void NqUtil::Split(LPCSTR s, char sep, xr_vector<xr_string>& out)
{
	out.clear();
	if (!s) return;
	xr_string cur;
	for (const char* p = s; ; ++p)
	{
		if (*p == sep || *p == 0)
		{
			xr_string t = Trim(cur);
			if (!t.empty()) out.push_back(t);
			cur.clear();
			if (*p == 0) break;
		}
		else cur += *p;
	}
}

u32 NqUtil::Fnv1a(LPCSTR s)
{
	u32 h = 2166136261u;
	for (const char* p = s ? s : ""; *p; ++p) { h ^= (unsigned char)*p; h *= 16777619u; }
	return h;
}

xr_string NqUtil::Format(LPCSTR fmt, ...)
{
	char buf[2048];
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	return xr_string(buf);
}
