# Luabind compatibility contract

This tree is the X-Ray `luabind.beta7-devel.rc4` lineage carried by the original
Dead Air and Call of Chernobyl source trees, with local compiler and allocator
adaptations. It is not an unmodified upstream Luabind release.

The audited modernization candidate was the existing DARF checkout of
`OpenXRay/luabind-deboostified` at commit
`d9d460f6120233a34ed54a3b01821e280cfce8c0` (`v0.9-368-gd9d460f`). The
candidate was used as source evidence only. No candidate source or binary is
vendored here.

The bundled Boost contract is `BOOST_VERSION=103000` (Boost 1.30.0) from
`SDK/Include/boost/version.hpp`.

## Required compatibility

Editor and game modules depend on behavior beyond the public upstream API:

- `luabind::functor` reference ownership, validity, reset, equality, and
  delayed proxy invocation when a return value is ignored.
- X-Ray allocator globals, allocator-backed `internal_*` containers, and no
  bypass of that allocator for Luabind-owned objects.
- `pcall_callback_fun` as `int (*)(lua_State*)`, the registration callback,
  and `luabind::error::state()` preserving the failing Lua stack.
- The current `class_rep`, `object_rep`, `object`, and `functor<void>` x64 MSVC
  layouts. Lua Studio and script-help code inspect representation details.
- `object_rep` ownership flags and raw-pointer/Lua-table access used by adopt,
  dependency, debugger, and userdata paths.
- Lua 5.1 stack, coroutine, bytecode, C-module, and ownership behavior for the
  CoP, CS, and SoC script-engine variants.
- Release builds defining both `DEBUG` and `NDEBUG`, matching the original
  X-Ray project configuration.

`link_compatibility.cpp` contains compile-time checks for the signatures,
allocator types, ownership flags, and x64 MSVC layouts that can be checked
without changing runtime behavior.

## Why the audited replacements are blocked

The deboostified candidate is not a drop-in implementation:

- It removes `luabind::functor`, the legacy allocator type/context API and
  `internal_*` container names, the registration callback, and
  debugger-visible representation APIs.
- Its protected-call callback is `void (*)(lua_State*)`; the current callback
  is the actual Lua error function and returns `int`.
- Its exception owns an allocator-backed message and does not expose the Lua
  state, changing error-stack handling.
- Its x64 layouts differ: `class_rep` 136 vs 584 bytes, `object_rep` 64 vs 72,
  and `object` 16 vs 24. The candidate has no corresponding
  `functor<void>` type; the current type is 24 bytes.
- Its userdata holder, ownership/adopt, and coroutine-resume behavior differs
  from the current X-Ray fork.
- Its debug/release configuration rejects simultaneous `DEBUG` and `NDEBUG`.
- Its class-registry cache is not suitable for backporting: lookup mutates an
  `unordered_map` without holding the cache mutex, uses a separate CRT
  allocation domain, and adds static-lifetime state to a library linked into
  multiple modules.

Boost cannot be replaced independently in this tree. Current Luabind uses the
Boost 1.30 `boost::function1` and `boost::function2` custom-allocator template
forms in 15 declarations. Boost 1.85 interprets that allocator argument as a
function parameter and fails compilation. Rewriting those types would change
`class_rep` layout and allocator behavior.

The generic Luabind target remains C++17. A standalone C++20 parse of this
fork fails in legacy template and header-order code, while editor consumers
already use C++20 through their established PCH boundary.

## Migration gate

A future replacement must pass old-versus-new golden tests before tracked
source changes:

1. Compile the public X-Ray API and all debugger representation consumers.
2. Match allocator/reallocator/free accounting with no cross-CRT ownership.
3. Match functor copy, reset, ignored-result invocation, error, and stack
   behavior.
4. Match owned, borrowed, const, held, adopted, and dependent userdata without
   leaks or double destruction.
5. Match protected-call callbacks, exception stack access, registration hooks,
   overload diagnostics, and coroutine yield/resume results.
6. Load representative original Dead Air source and Lua 5.1 bytecode/C modules
   in all CoP, CS, and SoC factories.
7. Build the Level Editor, repeat the no-work build, then run project, level,
   quest graph, selection, and read-only inspector regression tests.

Until every gate passes, update consumers behind an adapter or a versioned
boundary rather than replacing the linked implementation.
