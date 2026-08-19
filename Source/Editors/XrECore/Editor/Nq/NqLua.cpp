#include "stdafx.h"
#include "NqLua.h"
#include "NqUtil.h"
#include "NqCatalog.h"

extern "C" {
#include <lua/lua.h>
#include <lua/lauxlib.h>
#include <lua/luajit.h>
}

//==============================================================================
// reading
//==============================================================================
namespace
{
	const int	kMaxDepth		= 48;
	const int	kHookPeriod		= 10000;
	const int	kHookLimit		= 400;			// 4M instructions - a data file needs a few thousand

	struct SWalk
	{
		int	steps;
		SWalk() : steps(0) {}
	};

	// count hook: a data file that loops is a broken file, not a hang
	void CountHook(lua_State* L, lua_Debug*)
	{
		lua_pushstring(L, "execution limit exceeded (a quest file must be plain data)");
		lua_error(L);
	}

	// "=name:12: message" / "[string ...]:12: msg" -> line + message
	void SplitLuaError(LPCSTR raw, LPCSTR chunk, NqLua::SError& err)
	{
		err.line = 0; err.col = 0;
		err.message = raw ? raw : "unknown Lua error";
		if (!raw) return;
		const char* p = raw;
		if (chunk && chunk[0] && 0 == strncmp(raw, chunk, strlen(chunk))) p = raw + strlen(chunk);
		// the chunk name may also come back in [string "..."] form
		if (p == raw && raw[0] == '[')
		{
			if (const char* close = strstr(raw, "\"]")) p = close + 2;
		}
		if (*p == ':')
		{
			char* end = 0;
			const long line = strtol(p + 1, &end, 10);
			if (end && *end == ':' && line > 0)
			{
				err.line = (int)line;
				const char* msg = end + 1;
				while (*msg == ' ') ++msg;
				err.message = msg;
			}
		}
	}

	// column: position of the token quoted after "near '" in that source line
	void GuessColumn(LPCSTR text, u32 len, NqLua::SError& err)
	{
		if (err.line <= 0 || !text) return;
		const size_t near_at = err.message.find("near '");
		if (near_at == xr_string::npos) return;
		const size_t tok_beg = near_at + 6;
		const size_t tok_end = err.message.find('\'', tok_beg);
		if (tok_end == xr_string::npos || tok_end == tok_beg) return;
		xr_string tok = err.message.substr(tok_beg, tok_end - tok_beg);
		if (tok == "<eof>") return;
		// find the start of the line
		int line = 1;
		u32 i = 0;
		while (i < len && line < err.line) { if (text[i] == '\n') ++line; ++i; }
		u32 line_end = i;
		while (line_end < len && text[line_end] != '\n') ++line_end;
		xr_string src; src.assign(text + i, text + line_end);
		const size_t at = src.find(tok);
		if (at != xr_string::npos) err.col = (int)at + 1;
	}

	bool WalkValue(lua_State* L, int idx, SNqValue& out, int depth, SWalk& w, xr_string& err)
	{
		if (++w.steps > 200000) { err = "table too large"; return false; }
		switch (lua_type(L, idx))
		{
		case LUA_TNIL:		out = SNqValue::Nil();							return true;
		case LUA_TBOOLEAN:	out = SNqValue::Bool(!!lua_toboolean(L, idx));	return true;
		case LUA_TNUMBER:	out = SNqValue::Number(lua_tonumber(L, idx));	return true;
		case LUA_TSTRING:
			{
				size_t n = 0;
				const char* s = lua_tolstring(L, idx, &n);
				out = SNqValue::Nil();
				out.type = SNqValue::tString;
				out.s.assign(s, s + n);
				return true;
			}
		case LUA_TTABLE:
			{
				if (depth > kMaxDepth) { err = "table nesting too deep (self reference?)"; return false; }
				out = SNqValue::Table();
				// absolute index of the table (lua_next pushes on top of it)
				const int tabs = (idx > 0) ? idx : lua_gettop(L) + idx + 1;
				// pass 1: collect keys
				xr_vector<double>	num_keys;
				xr_vector<xr_string> str_keys;
				lua_pushnil(L);
				while (lua_next(L, tabs))
				{
					if (lua_type(L, -2) == LUA_TNUMBER)		num_keys.push_back(lua_tonumber(L, -2));
					else if (lua_type(L, -2) == LUA_TSTRING)
					{
						size_t n = 0;
						const char* s = lua_tolstring(L, -2, &n);
						{ xr_string k; k.assign(s, s + n); str_keys.push_back(k); }
					}
					// other key types (booleans, tables) are not data - dropped
					lua_pop(L, 1);
				}
				std::sort(num_keys.begin(), num_keys.end());
				std::sort(str_keys.begin(), str_keys.end(), NqKeyLess);
				// array part in numeric order (gaps are tolerated: a data file
				// with holes still reads as a list)
				for (u32 i = 0; i < num_keys.size(); ++i)
				{
					lua_pushnumber(L, num_keys[i]);
					lua_gettable(L, tabs);
					SNqValue v;
					const bool ok = WalkValue(L, -1, v, depth + 1, w, err);
					lua_pop(L, 1);
					if (!ok) return false;
					out.arr.push_back(v);
				}
				for (u32 i = 0; i < str_keys.size(); ++i)
				{
					lua_pushlstring(L, str_keys[i].c_str(), str_keys[i].size());
					lua_gettable(L, tabs);
					SNqValue v;
					const bool ok = WalkValue(L, -1, v, depth + 1, w, err);
					lua_pop(L, 1);
					if (!ok) return false;
					out.keys.push_back(str_keys[i]);
					out.vals.push_back(v);
				}
				return true;
			}
		default:
			err = "value is not plain data (function/userdata)";
			return false;
		}
	}
}

//------------------------------------------------------------------------------
// declarative source scan
//
// The empty environment only stops code that touches a global: `local x = 1`,
// literal arithmetic and concatenation would run silently and the file would
// pass as data here while the runtime rejects it with E003 (it scans the source
// the same way - xms_nq_load.script declarative_problem). Without this the build
// gate ships quests the game refuses to load.
//------------------------------------------------------------------------------
namespace
{
	IC bool DeclSpace(char c)	{ return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
	IC bool DeclDigit(char c)	{ return c >= '0' && c <= '9'; }
	IC bool DeclHex(char c)		{ return DeclDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
	IC bool DeclAlpha(char c)	{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
	IC bool DeclPunct(char c)	{ return c == '{' || c == '}' || c == '[' || c == ']' || c == '=' || c == ',' || c == ';' || c == '-'; }
	IC bool DeclLiteral(const xr_string& t) { return t == "true" || t == "false" || t == "nil"; }

	// s[i] == '[': level of the long bracket and the offset after it, or -1
	int LongBracketAt(LPCSTR s, u32 n, u32 i, u32& after)
	{
		u32 j = i + 1;
		int level = 0;
		while (j < n && s[j] == '=') { ++level; ++j; }
		if (j >= n || s[j] != '[') return -1;
		after = j + 1;
		return level;
	}

	bool SkipLong(LPCSTR s, u32 n, u32 from, int level, LPCSTR what, u32& next, xr_string& problem)
	{
		xr_string close = "]";
		for (int k = 0; k < level; ++k) close += '=';
		close += ']';
		const u32 cl = (u32)close.size();
		for (u32 j = from; j + cl <= n; ++j)
			if (0 == strncmp(s + j, close.c_str(), cl)) { next = j + cl; return true; }
		problem = xr_string("unterminated ") + what;
		return false;
	}

	// true + `problem` when the source is something other than one literal table
	bool DeclarativeProblem(LPCSTR s, u32 n, xr_string& problem)
	{
		problem.clear();
		xr_vector<xr_string> toks;
		u32 i = 0;
		while (i < n)
		{
			const char c = s[i];
			if (DeclSpace(c)) { ++i; continue; }
			if (c == '-' && i + 1 < n && s[i + 1] == '-')
			{
				u32 after = 0;
				const int level = (i + 2 < n && s[i + 2] == '[') ? LongBracketAt(s, n, i + 2, after) : -1;
				if (level >= 0) { if (!SkipLong(s, n, after, level, "long comment", i, problem)) return true; }
				else
				{
					while (i < n && s[i] != '\n') ++i;
					if (i < n) ++i;
				}
				continue;
			}
			if (c == '"' || c == '\'')
			{
				u32 j = i + 1;
				while (j < n)
				{
					if (s[j] == '\\')		j += 2;
					else if (s[j] == c || s[j] == '\n') break;
					else					++j;
				}
				if (j >= n || s[j] != c) { problem = "unterminated string"; return true; }
				toks.push_back("\1str");
				i = j + 1;
				continue;
			}
			if (c == '[')
			{
				u32 after = 0;
				const int level = LongBracketAt(s, n, i, after);
				if (level >= 0)
				{
					if (!SkipLong(s, n, after, level, "long string", i, problem)) return true;
					toks.push_back("\1str");
				}
				else { toks.push_back("["); ++i; }
				continue;
			}
			if (DeclDigit(c) || (c == '.' && i + 1 < n && DeclDigit(s[i + 1])))
			{
				u32 j = i;
				while (j < n)
				{
					const char d = s[j];
					if (DeclHex(d) || d == 'x' || d == 'X' || d == '.') ++j;
					else if ((d == '+' || d == '-') && j > i && (s[j - 1] == 'e' || s[j - 1] == 'E' || s[j - 1] == 'p' || s[j - 1] == 'P')) ++j;
					else break;
				}
				// "1 .. 2" reads as one "number" here: two dots mean concatenation
				for (u32 k = i; k + 1 < j; ++k)
					if (s[k] == '.' && s[k + 1] == '.') { problem = "concatenation is code, not data"; return true; }
				toks.push_back("\1num");
				i = j;
				continue;
			}
			if (DeclAlpha(c))
			{
				u32 j = i;
				while (j < n && (DeclAlpha(s[j]) || DeclDigit(s[j]))) ++j;
				xr_string word;
				word.assign(s + i, j - i);
				toks.push_back(word);
				i = j;
				continue;
			}
			if (DeclPunct(c)) { xr_string p; p += c; toks.push_back(p); ++i; continue; }
			problem = NqUtil::Format("'%c' outside a string is code, not data", c);
			return true;
		}
		if (toks.empty()) { problem = "no 'return' statement"; return true; }
		if (toks[0] != "return") { problem = NqUtil::Format("'%s' before the table is code, not data", toks[0].c_str()); return true; }
		for (u32 k = 1; k < toks.size(); ++k)
		{
			const xr_string& t = toks[k];
			if (t.empty() || !DeclAlpha(t[0]) || DeclLiteral(t)) continue;
			if (k + 1 < toks.size() && toks[k + 1] == "=") continue;
			problem = NqUtil::Format("'%s' is code, not data (only literals and 'key =' names are allowed)", t.c_str());
			return true;
		}
		return false;
	}
}

bool NqLua::ParseValue(LPCSTR text, u32 len, LPCSTR chunk_name, SNqValue& out, SError& err)
{
	out = SNqValue();
	err = SError();
	if (!text) { err.message = "empty text"; return false; }
	lua_State* L = luaL_newstate();
	if (!L) { err.message = "cannot create a Lua state"; return false; }
	// the count hook only fires in the interpreter
	luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);

	// the chunk name is a file name (Windows allows 255 characters), so it does
	// not go into a fixed buffer; Lua copies it
	const xr_string chunk = xr_string("=") + ((chunk_name && chunk_name[0]) ? chunk_name : "nqasset");
	bool ok = true;
	if (0 != luaL_loadbuffer(L, text, len, chunk.c_str()))
	{
		SplitLuaError(lua_tostring(L, -1), chunk.c_str() + 1, err);
		GuessColumn(text, len, err);
		ok = false;
	}
	else
	{
		// same order as the runtime: compile, then reject anything that is not a
		// literal table, then evaluate in an empty environment
		xr_string decl;
		if (DeclarativeProblem(text, len, decl))
		{
			err.message = decl;
			lua_close(L);
			return false;
		}
		lua_newtable(L);
		lua_setfenv(L, -2);
		lua_sethook(L, CountHook, LUA_MASKCOUNT, kHookPeriod * kHookLimit);
		const int base = lua_gettop(L) - 1;		// stack below the chunk
		if (0 != lua_pcall(L, 0, LUA_MULTRET, 0))
		{
			SplitLuaError(lua_tostring(L, -1), chunk.c_str() + 1, err);
			// a runtime error in a data file means code outside strings
			if (err.message.find("execution limit") == xr_string::npos)
				err.message = "code outside strings: " + err.message;
			ok = false;
		}
		// a second return value is code too, and lua_pcall with one result would
		// have swallowed it
		else if (lua_gettop(L) - base != 1 || !lua_istable(L, -1))
		{
			err.message = "the file must return one table (return { ... })";
			ok = false;
		}
		else
		{
			SWalk w;
			xr_string werr;
			if (!WalkValue(L, -1, out, 0, w, werr)) { err.message = werr; ok = false; }
		}
	}
	lua_close(L);
	return ok;
}

bool NqLua::SyntaxCheck(LPCSTR code, SError& err)
{
	err = SError();
	if (!code) return true;
	lua_State* L = luaL_newstate();
	if (!L) { err.message = "cannot create a Lua state"; return false; }
	const u32 len = xr_strlen(code);
	bool ok = true;
	if (0 != luaL_loadbuffer(L, code, len, "=code"))
	{
		SplitLuaError(lua_tostring(L, -1), "code", err);
		GuessColumn(code, len, err);
		ok = false;
	}
	lua_close(L);
	return ok;
}

//==============================================================================
// generic -> typed
//==============================================================================
static void ShapeErr(xr_vector<xr_string>& errs, LPCSTR fmt, ...)
{
	char buf[512];
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	errs.push_back(xr_string(buf));
}

bool NqLua::ActionFromValue(const SNqValue& v, SNqAction& a, xr_string& err)
{
	a = SNqAction();
	if (!v.IsTable())			{ err = "action must be a table { kind = ..., params = { ... } }"; return false; }
	const SNqValue* kind = v.Get("kind");
	if (!kind || !kind->IsString() || kind->s.empty())	{ err = "action without kind"; return false; }
	a.kind = kind->s;
	if (const SNqValue* p = v.Get("params"))
	{
		if (!p->IsTable() && !p->IsNil()) { err = "action params must be a table"; return false; }
		a.params = *p;
	}
	return true;
}

bool NqLua::CondFromValue(const SNqValue& v, SNqCond& c, xr_string& err)
{
	c = SNqCond();
	if (!v.IsTable())			{ err = "condition must be a table { kind = ..., params = { ... } }"; return false; }
	const SNqValue* kind = v.Get("kind");
	if (!kind || !kind->IsString() || kind->s.empty())	{ err = "condition without kind"; return false; }
	c.kind = kind->s;
	if (const SNqValue* p = v.Get("params"))
	{
		if (!p->IsTable() && !p->IsNil()) { err = "condition params must be a table"; return false; }
		c.params = *p;
	}
	if (const SNqValue* n = v.Get("not")) c.negate = n->AsBool(false);
	return true;
}

void NqLua::TaskFromValue(LPCSTR id, const SNqValue& v, SNqTask& t)
{
	t = SNqTask();
	t.id = id ? id : "";
	if (!v.IsTable()) return;
	if (const SNqValue* x = v.Get("title"))	t.title = *x;
	if (const SNqValue* x = v.Get("descr"))	t.descr = *x;
	t.type = v.GetString("type", "additional");
	if (const SNqValue* x = v.Get("target"))	t.target = *x;
	t.icon = v.GetString("icon", "");
	if (const SNqValue* objs = v.Get("objectives"))
		for (u32 i = 0; i < objs->arr.size(); ++i)
		{
			const SNqValue& o = objs->arr[i];
			SNqObjective obj;
			obj.id = o.GetString("id");
			if (const SNqValue* x = o.Get("title"))		obj.title = *x;
			if (const SNqValue* x = o.Get("descr"))		obj.descr = *x;
			if (const SNqValue* x = o.Get("target"))	obj.target = *x;
			t.objectives.push_back(obj);
		}
}

namespace
{
	int PinRank(const NqCatalog::SKind* k, const SNqNode& n, const xr_string& pin)
	{
		if (k)
			for (u32 i = 0; i < k->pins.size(); ++i)
			{
				if (k->pins[i] == pin) return (int)i * 1000;
				if (k->pins[i] != "cases") continue;
				const SNqValue* cases = n.params.Get("cases");
				if (!cases || !cases->IsTable()) continue;
				for (u32 c = 0; c < cases->arr.size(); ++c)
					if (cases->arr[c].GetString("name") == pin) return (int)i * 1000 + (int)c + 1;
			}
		return 1000000;
	}
}

void NqLua::CanonicalizePins(SNqNode& n)
{
	if (n.out.size() < 2) return;
	const NqCatalog::SKind* k = NqCatalog::Find(n.kind.c_str());
	// insertion sort keeps it stable and tiny
	for (u32 i = 1; i < n.out.size(); ++i)
	{
		u32 j = i;
		while (j > 0)
		{
			const int ra = PinRank(k, n, n.out[j].first), rb = PinRank(k, n, n.out[j - 1].first);
			const bool less = (ra != rb) ? ra < rb : n.out[j].first < n.out[j - 1].first;
			if (!less) break;
			std::swap(n.out[j], n.out[j - 1]);
			--j;
		}
	}
}

void NqLua::CanonicalizePins(SNqQuest& q)
{
	for (u32 i = 0; i < q.nodes.size(); ++i) CanonicalizePins(q.nodes[i]);
}

bool NqLua::NodeFromValue(const SNqValue& v, SNqNode& n, xr_vector<xr_string>& errs)
{
	n = SNqNode();
	if (!v.IsTable()) { ShapeErr(errs, "node must be a table"); return false; }
	const SNqValue* id = v.Get("id");
	if (!id || !id->IsString())	{ ShapeErr(errs, "node without id"); return false; }
	n.id = id->s;
	n.kind = v.GetString("kind", "");
	if (const SNqValue* o = v.Get("once"))
	{
		if (!o->IsBool()) ShapeErr(errs, "node '%s': once must be true or false", n.id.c_str());
		else { n.once = o->b; n.once_set = true; }
	}
	if (const SNqValue* p = v.Get("params"))
	{
		if (p->IsTable()) n.params = *p;
		else if (!p->IsNil()) ShapeErr(errs, "node '%s': params must be a table", n.id.c_str());
	}
	if (const SNqValue* c = v.Get("cond"))
	{
		if (!c->IsTable()) ShapeErr(errs, "node '%s': cond must be a list of conditions", n.id.c_str());
		else
		{
			// a single condition written without the outer list is accepted
			if (c->arr.empty() && c->Has("kind"))
			{
				SNqCond cc; xr_string e;
				if (CondFromValue(*c, cc, e)) n.cond.push_back(cc);
				else ShapeErr(errs, "node '%s': cond: %s", n.id.c_str(), e.c_str());
			}
			for (u32 i = 0; i < c->arr.size(); ++i)
			{
				SNqCond cc; xr_string e;
				if (CondFromValue(c->arr[i], cc, e)) n.cond.push_back(cc);
				else ShapeErr(errs, "node '%s': cond #%u: %s", n.id.c_str(), i + 1, e.c_str());
			}
		}
	}
	static const char* kSlots[2] = { "on_enter", "on_exit" };
	for (int s = 0; s < 2; ++s)
	{
		const SNqValue* lst = v.Get(kSlots[s]);
		if (!lst) continue;
		if (!lst->IsTable()) { ShapeErr(errs, "node '%s': %s must be a list of actions", n.id.c_str(), kSlots[s]); continue; }
		xr_vector<SNqAction>& dst = s ? n.on_exit : n.on_enter;
		if (lst->arr.empty() && lst->Has("kind"))
		{
			SNqAction a; xr_string e;
			if (ActionFromValue(*lst, a, e)) dst.push_back(a);
			else ShapeErr(errs, "node '%s': %s: %s", n.id.c_str(), kSlots[s], e.c_str());
		}
		for (u32 i = 0; i < lst->arr.size(); ++i)
		{
			SNqAction a; xr_string e;
			if (ActionFromValue(lst->arr[i], a, e)) dst.push_back(a);
			else ShapeErr(errs, "node '%s': %s #%u: %s", n.id.c_str(), kSlots[s], i + 1, e.c_str());
		}
	}
	if (const SNqValue* out = v.Get("out"))
	{
		if (!out->IsTable()) ShapeErr(errs, "node '%s': out must be a table { pin = target | { targets } }", n.id.c_str());
		else
		{
			if (!out->arr.empty()) ShapeErr(errs, "node '%s': out must use named pins", n.id.c_str());
			// pins are read in the canonical key order the walker produced;
			// the writer re-sorts by the catalog pin order anyway
			for (u32 i = 0; i < out->keys.size(); ++i)
			{
				const SNqValue& t = out->vals[i];
				xr_vector<xr_string> targets;
				if (t.IsString()) targets.push_back(t.s);
				else if (t.IsTable())
				{
					for (u32 k = 0; k < t.arr.size(); ++k)
					{
						if (t.arr[k].IsString()) targets.push_back(t.arr[k].s);
						else ShapeErr(errs, "node '%s': out.%s: target #%u is not a node id", n.id.c_str(), out->keys[i].c_str(), k + 1);
					}
				}
				else if (!t.IsNil()) ShapeErr(errs, "node '%s': out.%s must be a node id or a list of ids", n.id.c_str(), out->keys[i].c_str());
				if (!targets.empty()) n.out.push_back(SNqPin(out->keys[i], targets));
			}
		}
	}
	if (const SNqValue* pos = v.Get("pos"))
	{
		if (pos->IsTable() && pos->arr.size() >= 2)
		{
			n.pos.set((float)pos->arr[0].AsNumber(), (float)pos->arr[1].AsNumber());
			n.has_pos = true;
		}
		else if (pos->IsTable() && pos->Has("x") && pos->Has("y"))
		{
			n.pos.set((float)pos->GetNumber("x"), (float)pos->GetNumber("y"));
			n.has_pos = true;
		}
		else if (!pos->IsNil()) ShapeErr(errs, "node '%s': pos must be { x, y }", n.id.c_str());
	}
	n.comment = v.GetString("comment", "");
	CanonicalizePins(n);
	return true;
}

void NqLua::QuestFromValue(const SNqValue& v, SNqQuest& q, xr_vector<xr_string>& errs)
{
	q.Clear();
	if (!v.IsTable()) { ShapeErr(errs, "the file must return one table"); return; }
	if (const SNqValue* x = v.Get("nq"))
	{
		if (x->IsNumber()) q.nq = (int)x->n;
		else { q.nq = (int)x->AsNumber(-1); if (q.nq < 0) ShapeErr(errs, "nq must be a number"); }
	}
	else q.nq = 0;		// absent -> E001 by the validator
	q.id = v.GetString("id", "");
	if (const SNqValue* x = v.Get("title")) q.title = *x;
	q.activation = v.GetString("activation", "auto");
	if (const SNqValue* vars = v.Get("vars"))
	{
		if (!vars->IsTable()) ShapeErr(errs, "vars must be a table { name = default }");
		else
		{
			if (!vars->arr.empty()) ShapeErr(errs, "vars must use named keys");
			for (u32 i = 0; i < vars->keys.size(); ++i)
			{
				SNqVar var; var.name = vars->keys[i]; var.value = vars->vals[i];
				q.vars.push_back(var);
			}
		}
	}
	if (const SNqValue* tasks = v.Get("tasks"))
	{
		if (!tasks->IsTable()) ShapeErr(errs, "tasks must be a table { id = { title = ..., ... } }");
		else
		{
			if (!tasks->arr.empty()) ShapeErr(errs, "tasks must use named keys");
			for (u32 i = 0; i < tasks->keys.size(); ++i)
			{
				SNqTask t;
				if (!tasks->vals[i].IsTable()) ShapeErr(errs, "task '%s' must be a table", tasks->keys[i].c_str());
				TaskFromValue(tasks->keys[i].c_str(), tasks->vals[i], t);
				q.tasks.push_back(t);
			}
		}
	}
	if (const SNqValue* nodes = v.Get("nodes"))
	{
		if (!nodes->IsTable()) ShapeErr(errs, "nodes must be a list of node tables");
		else
		{
			for (u32 i = 0; i < nodes->arr.size(); ++i)
			{
				SNqNode n;
				if (NodeFromValue(nodes->arr[i], n, errs)) q.nodes.push_back(n);
				else if (!errs.empty())
				{
					char at[32]; sprintf_s(at, " (nodes #%u)", i + 1);
					errs.back() += at;
				}
			}
			if (!nodes->keys.empty()) ShapeErr(errs, "nodes must be a list, not a keyed table");
		}
	}
	else ShapeErr(errs, "nodes list is missing");
}

bool NqLua::ParseQuest(LPCSTR text, u32 len, LPCSTR chunk_name, SNqQuest& q, SError& err)
{
	SNqValue v;
	if (!ParseValue(text, len, chunk_name, v, err)) return false;
	xr_vector<xr_string> shape;
	QuestFromValue(v, q, shape);
	if (!shape.empty())
	{
		err = SError();
		for (u32 i = 0; i < shape.size(); ++i)
		{
			if (i) err.message += "\n";
			err.message += shape[i];
		}
		return false;
	}
	return true;
}

//==============================================================================
// typed -> generic
//==============================================================================
namespace
{
	// raw append: typed structs carry their own field order
	void Put(SNqValue& t, LPCSTR key, const SNqValue& v)
	{
		if (t.type == SNqValue::tNil) t.type = SNqValue::tTable;
		t.keys.push_back(xr_string(key));
		t.vals.push_back(v);
	}
}

void NqLua::ActionToValue(const SNqAction& a, SNqValue& out)
{
	out = SNqValue::Table();
	Put(out, "kind", SNqValue::String(a.kind));
	if (!a.params.IsEmpty()) Put(out, "params", a.params);
}

void NqLua::CondToValue(const SNqCond& c, SNqValue& out)
{
	out = SNqValue::Table();
	Put(out, "kind", SNqValue::String(c.kind));
	if (!c.params.IsEmpty()) Put(out, "params", c.params);
	if (c.negate) Put(out, "not", SNqValue::Bool(true));
}

void NqLua::NodeToValue(const SNqNode& n, SNqValue& out)
{
	out = SNqValue::Table();
	Put(out, "id", SNqValue::String(n.id));
	Put(out, "kind", SNqValue::String(n.kind));
	if (n.once_set) Put(out, "once", SNqValue::Bool(n.once));
	if (!n.params.IsEmpty()) Put(out, "params", n.params);
	if (!n.cond.empty())
	{
		SNqValue lst = SNqValue::Table();
		for (u32 i = 0; i < n.cond.size(); ++i) { SNqValue c; CondToValue(n.cond[i], c); lst.arr.push_back(c); }
		Put(out, "cond", lst);
	}
	if (!n.on_enter.empty())
	{
		SNqValue lst = SNqValue::Table();
		for (u32 i = 0; i < n.on_enter.size(); ++i) { SNqValue a; ActionToValue(n.on_enter[i], a); lst.arr.push_back(a); }
		Put(out, "on_enter", lst);
	}
	if (!n.on_exit.empty())
	{
		SNqValue lst = SNqValue::Table();
		for (u32 i = 0; i < n.on_exit.size(); ++i) { SNqValue a; ActionToValue(n.on_exit[i], a); lst.arr.push_back(a); }
		Put(out, "on_exit", lst);
	}
	bool any_out = false;
	for (u32 i = 0; i < n.out.size(); ++i) if (!n.out[i].second.empty()) any_out = true;
	if (any_out)
	{
		SNqValue o = SNqValue::Table();
		for (u32 i = 0; i < n.out.size(); ++i)
		{
			const xr_vector<xr_string>& t = n.out[i].second;
			if (t.empty()) continue;
			if (t.size() == 1) Put(o, n.out[i].first.c_str(), SNqValue::String(t[0]));
			else
			{
				SNqValue lst = SNqValue::Table();
				for (u32 k = 0; k < t.size(); ++k) lst.arr.push_back(SNqValue::String(t[k]));
				Put(o, n.out[i].first.c_str(), lst);
			}
		}
		Put(out, "out", o);
	}
	if (n.has_pos)
	{
		SNqValue p = SNqValue::Table();
		p.arr.push_back(SNqValue::Number(n.pos.x));
		p.arr.push_back(SNqValue::Number(n.pos.y));
		Put(out, "pos", p);
	}
	if (!n.comment.empty()) Put(out, "comment", SNqValue::String(n.comment));
}

void NqLua::QuestToValue(const SNqQuest& q, SNqValue& out)
{
	out = SNqValue::Table();
	Put(out, "nq", SNqValue::Number(q.nq));
	Put(out, "id", SNqValue::String(q.id));
	// an absent title stays absent: the runtime falls back to the id for a nil
	// title, but an empty string is a title, and the journal would show nothing
	if (!q.title.IsNil()) Put(out, "title", q.title);
	Put(out, "activation", SNqValue::String(q.activation));
	if (!q.vars.empty())
	{
		SNqValue vars = SNqValue::Table();
		for (u32 i = 0; i < q.vars.size(); ++i) Put(vars, q.vars[i].name.c_str(), q.vars[i].value);
		Put(out, "vars", vars);
	}
	if (!q.tasks.empty())
	{
		SNqValue tasks = SNqValue::Table();
		for (u32 i = 0; i < q.tasks.size(); ++i)
		{
			const SNqTask& t = q.tasks[i];
			SNqValue tv = SNqValue::Table();
			if (!t.title.IsNil()) Put(tv, "title", t.title);
			if (!t.descr.IsNil())	Put(tv, "descr", t.descr);
			Put(tv, "type", SNqValue::String(t.type.empty() ? "additional" : t.type.c_str()));
			if (!t.target.IsNil())	Put(tv, "target", t.target);
			if (!t.icon.empty())	Put(tv, "icon", SNqValue::String(t.icon));
			if (!t.objectives.empty())
			{
				// an array, because the order is the order the engine numbers them in
				SNqValue objs = SNqValue::Table();
				for (u32 j = 0; j < t.objectives.size(); ++j)
				{
					const SNqObjective& o = t.objectives[j];
					SNqValue ov = SNqValue::Table();
					Put(ov, "id", SNqValue::String(o.id));
					if (!o.title.IsNil())	Put(ov, "title", o.title);
					if (!o.descr.IsNil())	Put(ov, "descr", o.descr);
					if (!o.target.IsNil())	Put(ov, "target", o.target);
					objs.Push(ov);
				}
				Put(tv, "objectives", objs);
			}
			Put(tasks, t.id.c_str(), tv);
		}
		Put(out, "tasks", tasks);
	}
	SNqValue nodes = SNqValue::Table();
	xr_vector<int> order;
	q.TopoOrder(order);
	for (u32 i = 0; i < order.size(); ++i)
	{
		SNqValue nv;
		NodeToValue(q.nodes[order[i]], nv);
		nodes.arr.push_back(nv);
	}
	Put(out, "nodes", nodes);
}

//==============================================================================
// writing
//==============================================================================
namespace
{
	const u32 kWidth = 100;

	bool IsLuaKeyword(LPCSTR s)
	{
		static const char* kw[] =
		{
			"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
			"local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", "goto",
		};
		for (int i = 0; i < (int)(sizeof(kw) / sizeof(kw[0])); ++i) if (0 == strcmp(s, kw[i])) return true;
		return false;
	}

	bool IsIdent(LPCSTR s)
	{
		if (!s || !s[0]) return false;
		if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) return false;
		for (const char* p = s + 1; *p; ++p)
			if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
		return !IsLuaKeyword(s);
	}

	void FormatNumber(xr_string& out, double n)
	{
		char buf[64];
		if (n == (double)(long long)n && fabs(n) < 9.0e15) { sprintf_s(buf, "%lld", (long long)n); out += buf; return; }
		// values that are exactly a float (canvas positions) print at float
		// precision, so 12.5f does not come out as 12.5000000004
		if (n == (double)(float)n)
		{
			sprintf_s(buf, "%.7g", n);
			if ((float)strtod(buf, 0) == (float)n) { out += buf; return; }
		}
		sprintf_s(buf, "%.15g", n);
		if (strtod(buf, 0) != n) sprintf_s(buf, "%.17g", n);
		out += buf;
	}

	// a string goes into a long bracket when it holds newlines and nothing the
	// lexer would normalise away (\r) or that needs an escape anyway
	bool WantsLongBracket(const xr_string& s)
	{
		bool nl = false;
		for (u32 i = 0; i < s.size(); ++i)
		{
			const unsigned char c = (unsigned char)s[i];
			if (c == '\n') nl = true;
			else if (c == '\r') return false;
			else if (c < 0x20 && c != '\t') return false;
		}
		return nl;
	}

	void QuoteString(xr_string& out, const xr_string& s)
	{
		out += '"';
		for (u32 i = 0; i < s.size(); ++i)
		{
			const unsigned char c = (unsigned char)s[i];
			switch (c)
			{
			case '"':	out += "\\\"";	break;
			case '\\':	out += "\\\\";	break;
			case '\n':	out += "\\n";	break;
			case '\r':	out += "\\r";	break;
			case '\t':	out += "\\t";	break;
			default:
				if (c < 0x20 || c == 0x7F)
				{
					char b[8]; sprintf_s(b, "\\%03d", (int)c); out += b;
				}
				else out += (char)c;
			}
		}
		out += '"';
	}

	void LongString(xr_string& out, const xr_string& s)
	{
		// pick a level whose closing bracket does not occur in the text
		int level = 0;
		// text ending in ']' merges with a level-0 closer ("abc]" + "]]" reads as "abc" plus a
		// stray ']'), so such a string needs at least one '=' in the delimiter
		if (!s.empty() && ']' == s[s.size() - 1]) level = 1;
		for (;;)
		{
			xr_string close = "]";
			for (int i = 0; i < level; ++i) close += '=';
			close += ']';
			if (s.find(close) == xr_string::npos) break;
			++level;
		}
		out += '[';
		for (int i = 0; i < level; ++i) out += '=';
		out += "[\n";
		out += s;
		out += ']';
		for (int i = 0; i < level; ++i) out += '=';
		out += ']';
	}

	void WriteKey(xr_string& out, const xr_string& k)
	{
		if (IsIdent(k.c_str())) { out += k; return; }
		out += '[';
		QuoteString(out, k);
		out += ']';
	}

	// single-line form; false when a value needs a long bracket (multiline)
	bool Inline(xr_string& out, const SNqValue& v)
	{
		switch (v.type)
		{
		case SNqValue::tNil:	out += "nil";	return true;
		case SNqValue::tBool:	out += v.b ? "true" : "false"; return true;
		case SNqValue::tNumber:	FormatNumber(out, v.n); return true;
		case SNqValue::tString:
			if (WantsLongBracket(v.s)) return false;
			QuoteString(out, v.s);
			return true;
		case SNqValue::tTable:
			{
				if (v.arr.empty() && v.keys.empty()) { out += "{}"; return true; }
				out += "{ ";
				bool first = true;
				for (u32 i = 0; i < v.arr.size(); ++i)
				{
					if (!first) out += ", ";
					first = false;
					if (!Inline(out, v.arr[i])) return false;
				}
				for (u32 i = 0; i < v.keys.size(); ++i)
				{
					if (!first) out += ", ";
					first = false;
					WriteKey(out, v.keys[i]);
					out += " = ";
					if (!Inline(out, v.vals[i])) return false;
				}
				out += " }";
				return true;
			}
		}
		return true;
	}

	void Indent(xr_string& out, int n) { for (int i = 0; i < n; ++i) out += ' '; }

	// pretty form: inline when it fits, one entry per line otherwise
	void Pretty(xr_string& out, const SNqValue& v, int indent, u32 first_line_used)
	{
		if (v.type != SNqValue::tTable)
		{
			if (v.type == SNqValue::tString && WantsLongBracket(v.s)) { LongString(out, v.s); return; }
			Inline(out, v);
			return;
		}
		xr_string one;
		if (Inline(one, v) && first_line_used + one.size() <= kWidth) { out += one; return; }
		if (v.arr.empty() && v.keys.empty()) { out += "{}"; return; }
		out += "{\n";
		for (u32 i = 0; i < v.arr.size(); ++i)
		{
			Indent(out, indent + 2);
			Pretty(out, v.arr[i], indent + 2, indent + 2);
			out += ",\n";
		}
		for (u32 i = 0; i < v.keys.size(); ++i)
		{
			Indent(out, indent + 2);
			xr_string key;
			WriteKey(key, v.keys[i]);
			out += key;
			out += " = ";
			Pretty(out, v.vals[i], indent + 2, indent + 2 + (u32)key.size() + 3);
			out += ",\n";
		}
		Indent(out, indent);
		out += "}";
	}

	// node layout: "{ id = ..., kind = ...[, once = ...]," then one field per
	// line at indent+2, closing "}" on the last field's line; a node whose
	// whole inline form fits stays on one line
	void WriteNodeAt(xr_string& out, const SNqValue& nv, int indent)
	{
		xr_string one;
		if (Inline(one, nv) && indent + one.size() <= kWidth) { out += one; return; }
		out += "{ ";
		u32 i = 0;
		// head: id, kind, once
		for (; i < nv.keys.size(); ++i)
		{
			const xr_string& k = nv.keys[i];
			if (k != "id" && k != "kind" && k != "once") break;
			if (i) out += ", ";
			out += k; out += " = ";
			Inline(out, nv.vals[i]);
		}
		if (i >= nv.keys.size()) { out += " }"; return; }
		out += ",\n";
		for (; i < nv.keys.size(); ++i)
		{
			Indent(out, indent + 2);
			xr_string key;
			WriteKey(key, nv.keys[i]);
			out += key;
			out += " = ";
			Pretty(out, nv.vals[i], indent + 2, indent + 2 + (u32)key.size() + 3);
			out += (i + 1 < nv.keys.size()) ? ",\n" : " }";
		}
	}

	xr_string OneLine(const xr_string& s)
	{
		xr_string r;
		for (u32 i = 0; i < s.size(); ++i)
		{
			const unsigned char c = (unsigned char)s[i];
			r += (c == '\n' || c == '\r' || c == '\t') ? ' ' : (char)c;
		}
		return r;
	}
}

xr_string NqLua::WriteValue(const SNqValue& v, int indent)
{
	xr_string out;
	Pretty(out, v, indent, indent);
	return out;
}

xr_string NqLua::WriteNode(const SNqNode& n, int indent)
{
	SNqValue nv;
	NodeToValue(n, nv);
	xr_string out;
	WriteNodeAt(out, nv, indent);
	return out;
}

xr_string NqLua::Write(const SNqQuest& q)
{
	xr_string out;
	// header: the only comments the editor writes; ignored on read
	out += "-- nq: ";
	out += q.id;
	out += " \"";
	out += OneLine(NqText::Preview(q.title));
	out += "\"\n";
	{
		xr_string ol = Outline(q);
		size_t pos = 0;
		while (pos < ol.size())
		{
			size_t eol = ol.find('\n', pos);
			if (eol == xr_string::npos) eol = ol.size();
			out += "-- ";
			out += ol.substr(pos, eol - pos);
			out += "\n";
			pos = eol + 1;
		}
	}
	out += "return {\n";
	SNqValue qv;
	QuestToValue(q, qv);
	for (u32 i = 0; i < qv.keys.size(); ++i)
	{
		const xr_string& k = qv.keys[i];
		const SNqValue& v = qv.vals[i];
		out += "  "; out += k; out += " = ";
		if (k == "nodes")
		{
			out += "{\n";
			for (u32 n = 0; n < v.arr.size(); ++n)
			{
				if (n) out += "\n";
				Indent(out, 4);
				WriteNodeAt(out, v.arr[n], 4);
				out += ",\n";
			}
			out += "  }";
		}
		else if (k == "tasks")
		{
			// one task per line when the whole table does not fit
			xr_string one;
			if (Inline(one, v) && 2 + k.size() + 3 + one.size() <= kWidth) out += one;
			else
			{
				out += "{\n";
				for (u32 t = 0; t < v.keys.size(); ++t)
				{
					Indent(out, 4);
					xr_string key; WriteKey(key, v.keys[t]);
					out += key; out += " = ";
					Pretty(out, v.vals[t], 4, 4 + (u32)key.size() + 3);
					out += ",\n";
				}
				out += "  }";
			}
		}
		else Pretty(out, v, 2, 2 + (u32)k.size() + 3);
		out += ",\n";
	}
	out += "}\n";
	return out;
}

//==============================================================================
// outline
//==============================================================================
namespace
{
	xr_string ParamsSummary(const SNqValue& params, bool skip_text)
	{
		xr_string s;
		if (!params.IsTable()) return s;
		for (u32 i = 0; i < params.keys.size(); ++i)
		{
			if (skip_text && params.keys[i] == "text") continue;
			if (!s.empty()) s += ", ";
			s += params.keys[i];
			s += "=";
			const SNqValue& v = params.vals[i];
			if (params.keys[i] == "code")	s += "lua";
			else if (params.keys[i] == "cases" && v.IsTable())
			{
				for (u32 c = 0; c < v.arr.size(); ++c)
				{
					if (c) s += "|";
					s += v.arr[c].GetString("name", "?");
				}
			}
			else if (v.IsString())			s += OneLine(NqUtil::ClipUtf8(v.s, 40));
			else							s += OneLine(NqText::ValueSummary(v, 40));
		}
		return s;
	}

	xr_string ActionsSummary(const xr_vector<SNqAction>& lst)
	{
		xr_string s;
		for (u32 i = 0; i < lst.size(); ++i)
		{
			if (i) s += ", ";
			s += lst[i].kind;
			s += "(";
			s += ParamsSummary(lst[i].params, false);
			s += ")";
		}
		return s;
	}

	xr_string CondsSummary(const xr_vector<SNqCond>& lst)
	{
		xr_string s;
		for (u32 i = 0; i < lst.size(); ++i)
		{
			if (i) s += " and ";
			if (lst[i].negate) s += "not ";
			s += lst[i].kind;
			s += "(";
			s += ParamsSummary(lst[i].params, false);
			s += ")";
		}
		return s;
	}
}

xr_string NqLua::OutlineNode(const SNqQuest& q, const SNqNode& n)
{
	(void)q;
	xr_string line;
	const bool trig = NqText::IsTrigger(n.kind.c_str());
	if (trig) line += "[T] ";
	line += n.id;
	line += " (";
	line += n.kind;
	xr_string ps = ParamsSummary(n.params, true);
	if (!ps.empty()) { line += "; "; line += ps; }
	if (n.once_set) line += n.once ? "; once" : "; once=false";
	line += ")";
	if (const SNqValue* text = n.params.Get("text"))
	{
		line += " \"";
		line += OneLine(NqText::Preview(*text));
		line += "\"";
	}
	if (!n.cond.empty())		{ line += "  cond: ";		line += CondsSummary(n.cond); }
	if (!n.on_enter.empty())	{ line += "  on_enter: ";	line += ActionsSummary(n.on_enter); }
	if (!n.on_exit.empty())		{ line += "  on_exit: ";	line += ActionsSummary(n.on_exit); }
	bool any = false;
	for (u32 p = 0; p < n.out.size(); ++p)
	{
		const xr_vector<xr_string>& t = n.out[p].second;
		if (t.empty()) continue;
		any = true;
		// "[T] start (trigger.start) -> meet" / "sid_1 (...)  next -> accept, decline"
		if (trig && n.out.size() == 1) line += " -> ";
		else { line += "  "; line += n.out[p].first; line += " -> "; }
		for (u32 k = 0; k < t.size(); ++k) { if (k) line += ", "; line += t[k]; }
	}
	if (!any) line += "  (leaf)";
	return line;
}

xr_string NqLua::Outline(const SNqQuest& q)
{
	xr_string out = "quest ";
	out += q.id;
	out += " \"";
	out += OneLine(NqText::Preview(q.title));
	out += "\" (active: ";
	out += q.activation;
	if (!q.vars.empty())
	{
		out += "; vars: ";
		for (u32 i = 0; i < q.vars.size(); ++i) { if (i) out += ", "; out += q.vars[i].name; }
	}
	if (!q.tasks.empty())
	{
		out += "; tasks: ";
		for (u32 i = 0; i < q.tasks.size(); ++i) { if (i) out += ", "; out += q.tasks[i].id; }
	}
	out += ")\n";
	xr_vector<int> order;
	q.TopoOrder(order);
	for (u32 i = 0; i < order.size(); ++i)
	{
		out += OutlineNode(q, q.nodes[order[i]]);
		out += "\n";
	}
	return out;
}

//==============================================================================
// files
//==============================================================================
bool NqLua::ReadFile(LPCSTR path, xr_string& text, xr_string& err)
{
	text.clear(); err.clear();
	HANDLE h = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							 FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) { err = "cannot open file"; return false; }
	LARGE_INTEGER size;
	if (!::GetFileSizeEx(h, &size) || size.QuadPart > (16 << 20)) { ::CloseHandle(h); err = "file too large"; return false; }
	xr_vector<char> buf((size_t)size.QuadPart + 1, 0);
	DWORD got = 0;
	if (size.QuadPart && !::ReadFile(h, buf.data(), (DWORD)size.QuadPart, &got, NULL)) { ::CloseHandle(h); err = "read error"; return false; }
	::CloseHandle(h);
	text.assign(buf.data(), buf.data() + got);
	// a UTF-8 BOM is not part of the text
	if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
		text.erase(0, 3);
	return true;
}

bool NqLua::WriteFile(LPCSTR path, const xr_string& text, xr_string& err)
{
	err.clear();
	// create the directory chain
	{
		string_path dir;
		strncpy_s(dir, sizeof(dir), path, _TRUNCATE);
		for (char* p = dir + 3; *p; ++p)
			if (*p == '\\' || *p == '/')
			{
				const char c = *p; *p = 0;
				::CreateDirectoryA(dir, NULL);
				*p = c;
			}
	}
	HANDLE h = ::CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) { err = "cannot create file"; return false; }
	DWORD wr = 0;
	const bool ok = text.empty() || (::WriteFile(h, text.c_str(), (DWORD)text.size(), &wr, NULL) && wr == text.size());
	::CloseHandle(h);
	if (!ok) err = "write error";
	return ok;
}

u64 NqLua::FileTime(LPCSTR path)
{
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!::GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
	return ((u64)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
}
