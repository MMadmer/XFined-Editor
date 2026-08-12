# XFined Editor

Level/SDK editor for **Dead Air: Refined** (S.T.A.L.K.E.R. X-Ray 1.6 family),
built on the [RedPanda XRayEngine](https://github.com/RedPandaProjects/XRayEngine)
port of the original GSC editors to x64 + ImGui.

### What it adds on top of RedPanda

* **Mod projects** — pick/create a project folder on startup; all writes
  (scenes, compiled spawns, imports, logs) land in the project, the shared SDK
  data stays read-only. `File → Project → Import Base Scene` copies a stock
  level into the project.
* **Unreal-style viewport** — RMB fly + WASD/QE, MMB pan, wheel dolly/speed,
  Alt orbit; Q/W/E/R gizmo modes, Space cycle, F focus, Esc deselect;
  click-select across all object classes.
* **Content Browser** — dockable asset browser with folder tree, thumbnail
  grid, search, double-click placement and drag&drop into the viewport.
* Per-monitor DPI awareness, GPU picker for hybrid laptops
  (`Options → Render → GPU`), CoC/Dead Air data compatibility fixes.

### Building

One command after a clean clone (needs Visual Studio 2022+ with the
*Desktop development with C++* workload; CMake and Ninja ship with VS):

```powershell
.\Build.ps1
```

Binaries land in `Bin\x64\Release`. Useful variants: `-Target XrGame`,
`-Clean`, `-Debug`. Optional targets: `XrLC`, `xrDO_Light` (level compilers).

### Command line

```powershell
LevelEditor.exe -project "D:\XFinedProjects\Test"
```

`-project <folder|name>` opens that project at startup and **skips the project
picker** — the point being that a scripted run (a smoke test, an AI agent
driving the MCP port) lands straight in the project it needs instead of sitting
on a modal nobody is there to click. The argument is either a project folder or
the name of a project already in the recent list; quote it when it has spaces.
An unopenable value logs why and falls back to the picker.

Diagnostics, all off by default: `-flushlog` (write the log line by line, so a
crash keeps what was already printed), `-trace` (frame markers plus the
capture/mirror reasons), `-bc` (allocation-free breadcrumbs to `bc.txt`),
`-debugrender` (drain the D3D11 InfoQueue into the log).

### AI integration (MCP)

The editor hosts a local automation endpoint on `127.0.0.1:28016` from the
first frame (project browser included). A dependency-free MCP stdio bridge
lives at `tools/mcp/xfined_mcp.py`; tools include editor/viewport screenshots
(working even when the window is covered), content-browser navigation, asset
copying out of the read-only sources, project/file management and editor state
queries. Combine `-project` with the bridge to get a fully unattended session.

Setup instructions for any AI agent — including a ready-to-paste prompt block —
are in [`tools/mcp/AI_SETUP.md`](tools/mcp/AI_SETUP.md). Claude Code one-liner:

```bash
claude mcp add -s user xfined-editor -- python "<repo>/tools/mcp/xfined_mcp.py"
```

### Credits

* GSC Game World — the original X-Ray engine and editors. This project is not
  sanctioned by GSC Game World; they remain the copyright holders of the
  original source code.
* [RedPanda Projects](https://github.com/RedPandaProjects/XRayEngine) — the
  x64/ImGui port this fork is based on.
