# LuaJIT provenance and compatibility contract

This directory is an exact source snapshot of the local OpenXRay LuaJIT fork:

- repository: `https://github.com/OpenXRay/LuaJIT.git`
- commit: `ade7495df85bf755dc11c0b10d7515227a5215c1`
- describe: `v2.1.0-beta3-246-gade7495d`
- local source: `D:\Games\Dead Air\DeadAir-x64\Externals\LuaJIT`
- imported paths: `COPYRIGHT`, `README`, `src`, and `dynasm`

`UPSTREAM_BLOBS.sha1` records the Git mode, object type, SHA-1, and path for
all 210 imported files. Its non-comment lines are the exact output of:

```powershell
git -C 'D:\Games\Dead Air\DeadAir-x64\Externals\LuaJIT' ls-tree -r ade7495df85bf755dc11c0b10d7515227a5215c1 -- COPYRIGHT README src dynasm
```

The snapshot includes OpenXRay commit
`434fec1a11d594302fefe1d1b86709540874e79d`, which translates legacy LuaJIT
bytecode version 1 during loading, and commit
`ade7495df85bf755dc11c0b10d7515227a5215c1`, which fixes x64 diagnostics.
The CMake generation logic was adapted from the local DARF superproject at
commit `7715680ef36a62504cf485da81bfafa98854ceec` without modifying the vendored
LuaJIT sources.

The editor build intentionally preserves the original Dead Air scripting
contract:

- Windows x64 DLL and import-library basename: `lua51`
- Lua 5.1 C API and LuaJIT bytecode loading
- FR1 layout with `LUAJIT_DISABLE_GC64`
- JIT and FFI enabled
- Lua 5.2 compatibility mode disabled
- bundled low-address allocator enabled
- public headers copied byte-for-byte from this snapshot

Do not update this dependency or change these flags without rerunning the
old/new bytecode, C-module, allocator, coroutine, error, userdata, hook, and
binding compatibility corpus. Luabind is a separate dependency and is not
part of this migration.
