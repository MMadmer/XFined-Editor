# XFined Editor — MCP setup for AI agents

Copy-paste this file (or its relevant part) into any AI assistant that supports
MCP. It contains everything needed to connect to and control the editor.

## What is available

The XFined Editor hosts a local automation endpoint whenever it is running:

* transport: TCP, line-based JSON, `127.0.0.1:28016`
* alive from the very first frame — the project browser stage included
* an MCP stdio bridge is bundled at `tools/mcp/xfined_mcp.py` (Python 3, stdlib only)

## Starting the editor on a project

Launch it with the project already open and the picker skipped — otherwise an
unattended run stops on a modal nobody is there to click:

```powershell
LevelEditor.exe -project "D:\XFinedProjects\Test"
```

`-project` takes a project folder or the name of one in the recent list (quote
paths with spaces). An unopenable value is logged and the picker comes up as
usual. `xfined_open_project` does the same thing on an editor that is already
running.

**Always add `-nodlg` to an unattended run.** A fatal error then never opens a
system-modal dialog: the log gets a real, symbolised stack trace, a minidump is
written next to the logs, and the process terminates (exit code 3) instead of
hanging on a box nobody will click. Also useful: `-flushlog` (log written line
by line, so a crash keeps what was printed), `-trace` (frame markers and
capture diagnostics).

## Registering the MCP server

**Claude Code:**

```bash
claude mcp add -s user xfined-editor -- python "<repo>/tools/mcp/xfined_mcp.py"
```

**Any other MCP client** — generic server config:

```json
{
  "mcpServers": {
    "xfined-editor": {
      "command": "python",
      "args": ["<repo>/tools/mcp/xfined_mcp.py"]
    }
  }
}
```

Replace `<repo>` with the folder the editor is installed in.

## Tools

| Tool | What it returns |
|---|---|
| `xfined_ping` | editor presence check |
| `xfined_state` | active project name/path, opened scene, FPS |
| `xfined_screenshot_viewport` | PNG image of the 3D viewport render target |
| `xfined_screenshot_editor` | PNG image of the whole editor window (all panels), read from the presented frame, so it works even when the editor is covered by other windows. Mirroring the back buffer costs a full readback, so it only runs while armed: **the first call after an idle spell answers "capture just armed" — call it again** and the second one returns the image |
| `xfined_scene_info` | opened scene file, modified flag, object counts per tool class |
| `xfined_selection` | objects selected in the viewport: name, class, position/rotation/scale |
| `xfined_object_info` | one object by exact `name`: class, selected/visible, transform |
| `xfined_list_projects` | recent projects (name + folder path) |
| `xfined_open_project` | open a project by `path`; its last scene auto-loads |
| `xfined_open_scene` | load a `.level` scene in the active project (synchronous) |
| `xfined_list_objects` | scene object names; filters: `class`, `name_contains`, `limit` |
| `xfined_focus_object` | frame the camera on object `name` like the F key; `select=true` also selects it |
| `xfined_mod_manifest` | XMS mod manifest of the active project: id, name, version, target game `mode`, declared `provides_mode` list, requires/after/before/conflicts, existing module dirs |
| `xfined_mod_set_manifest` | update mod.ltx fields: `id`/`name`/`version`/`mode` strings, `provides_mode_id`+`provides_mode_title` (declare a game mode, empty id removes), `requires`/`after`/`before`/`conflicts` comma-joined lists |
| `xfined_mod_export` | export the project to `<target>/mods/<id>/` as an XMS module, or `flat=true` for a plain `gamedata_<id>` overlay + compatibility report |
| `xfined_mod_export_spawn` | export placed objects as an additive spawn layer `spawn/<level>.xspawn` (`op=add` + section + world transform + visual override) — the composable way to add things to a base level; args `level`, `selected_only`, `section`, `mode` |
| `xfined_asset_preview_model` | render a `.ogf` model preview as a base64 PNG (game meshes have no baked thumbnail); `name` + `source` = `visual` (editor FS) or `darf` (linked game install) |
| `xfined_mod_export_cut` | subtractive delta: selected objects act as volumes — their bounds become cut boxes (base collision removed) + `hide` entries (base visuals detached); args `level`, `selected_only`, `overlap`, `grow` |
| `xfined_mod_export_xcform` | bake scene objects into `levels/<level>/overlay.xcform` (world-space collision triangles + game materials); args `level`, `sector`, `selected_only` |
| `xfined_mod_export_ogf_probe` | export selected static objects as world-space `.ogf` into `levels/<level>/visuals/` + register them in `overlay_visuals.ltx`; skeleton/dynamic refs are skipped |
| `xfined_game_link` | report the project's linked game install (`linked`, `game_root`, `reason`), or link one by `path` (needs fsgame.ltx + `database\` or `gamedata\`); the game folder is only ever read |
| `xfined_darf_list` | files of the linked game install (`database\*.xdb*` archives + loose `gamedata\`) as one gamedata-relative listing; filters `filter`, `ext`, `limit` (default 200); each entry has `path`, `size`, `source` (`archive`/`loose`) |
| `xfined_darf_copy` | copy one game file (`path` from `darf_list`) into `<project>/gamedata/<same relative path>`; `overwrite` defaults to false and an existing target is reported instead of replaced |
| `xfined_scene_stats` | scene_info + selection count + camera position/rotation |
| `xfined_camera_get` | camera position, `hpb` rotation (radians), view direction |
| `xfined_camera_set` | move/rotate the camera: `x`/`y`/`z`, `h`/`p`/`b`; omitted fields stay |
| `xfined_select_objects` | select objects by exact `names` (comma list); clears previous selection, returns missing names |
| `xfined_deselect_all` | clear the selection in every tool |
| `xfined_object_set_position` | set object position by `name` (`x`/`y`/`z`, partial ok); undoable |
| `xfined_object_set_rotation` | set object rotation in radians by `name` (`x`/`y`/`z`, partial ok); undoable |
| `xfined_object_set_scale` | set object scale by `name` (`x`/`y`/`z`, partial ok); undoable |
| `xfined_place_object` | place a library object (`ref`) with a full transform: `x/y/z`, `rx/ry/rz` (radians), `sx/sy/sz` or `scale`, plus `snap_to_ground`/`align_normal`; no position = in front of the camera |
| `xfined_drop_objects` | drop objects onto the surface below (the End key, Unreal Snap-to-Floor semantics); `mode` `box` (swept bounds, default) or `line` (pivot ray), `names` comma list or the selection, `align_normal` |
| `xfined_view_mode` | read/set viewport rendering: `preset` (`unlit` = normal textured view, `lit` = 8-slot fixed-function lighting so most geometry goes black, `wireframe`, `point`), `fill`, `shade`, and the raw `lighting`/`textures`/`edged_faces`/`filter_linear`/`fog`/`environment`/`grid`/`safe_rect` toggles |
| `xfined_list_assets` | list available assets: `category` (Objects by default), `filter`, `limit`; names feed `place_object`'s `ref` |
| `xfined_asset_preview` | PNG thumbnail of one asset (`name`, optional `category`) for judging its look before placing |
| `xfined_rename_object` | rename a scene object (`name` -> `new_name`, must be unique) |
| `xfined_delete_selected` | delete every selected object; returns the removed count; undoable |
| `xfined_save_scene` | save the opened scene (its current file, or an explicit `file`) |
| `xfined_outliner_show` | open (`open=true`, default) or close the World Outliner panel |
| `xfined_outliner_filter` | drive the outliner's search box and type funnel: `text` (Unreal grammar - every word must match, `-word` excludes, `"two words"` match together), `types` (`;`-separated classes to show, empty shows all), `selected_only`; returns `shown`/`total` |
| `xfined_scene_tree` | the World Outliner's data: groups per object tool class sorted by class name, each with `count`/`total` and `objects` (name, selected, visible); filters `filter`, `class`, `limit` (default 200) |
| `xfined_content_browser_open` | reveal an asset in the Content Browser: opens the panel, switches `source` (`project`/`editor`/`darf`), navigates to its folder and selects it; `open=true` also opens its viewer (the double-click action) |
| `xfined_content_browser_selection` | what the browser shows right now: open flag, active source, current folder, selected assets |
| `xfined_content_browser_copy` | copy assets out of a READ-ONLY source into the project: `names` (`;`-separated) or `folder` (recursive), optional `source`/`category`, `overwrite` (default true). With `dst` they land in that project-relative folder, a folder keeping its own name and subtree — the same clipboard and paste the browser's menu uses. Without `dst` each asset is mirrored where the engine expects it instead: an Objects entry in `<project>/rawdata/objects/...`, a mesh in `<project>/gamedata/meshes/...`, a texture with its `.thm` |
| `xfined_content_copy` / `xfined_content_move` | copy/move files or folders by path; `src` accepts a `;`-separated list, `dst` is a folder inside the project, `recursive`/`overwrite`. Moving out of a read-only source is refused — a move deletes its source |
| `xfined_content_delete` | delete files/folders inside the project; `recursive` defaults to false so a non-empty folder is reported instead of wiped |
| `xfined_content_mkdir` | create a folder (with missing parents) inside the project |
| `xfined_list_commands` | the editor's own command registry: every action the menus and shortcuts can perform, as `{id, name, menu, presets}` — the editor's full control surface, and commands added in future builds appear here automatically |
| `xfined_exec_command` | execute ANY registry command — the same dispatch menus and shortcuts use. Address by `id`, `COMMAND_*` `name` or menu path; `p1s`/`p2s` pass strings, `p1i`/`p2i` integers. Prefer a dedicated tool when one exists; this is the escape hatch for everything else |
| `xfined_undo` | undo the last scene operation |
| `xfined_redo` | redo the last undone scene operation |

Everything that writes is clamped to the project folder: the linked game install
and the shared SDK library are sources you copy **out of**, never into.

## Raw protocol (if you don't want the bridge)

One JSON object per line in, one per line out:

```
> {"cmd":"ping"}
< {"ok":true,"name":"XFined Editor","version":"1.0"}

> {"cmd":"state"}
< {"ok":true,"project_active":true,"project":"D:/Mods/MyMod","project_name":"MyMod","scene":"...","fps":240}

> {"cmd":"screenshot_viewport"}
< {"ok":true,"png_base64":"iVBORw0KGgo..."}

> {"cmd":"screenshot_editor"}
< {"ok":true,"png_base64":"iVBORw0KGgo..."}
```

Errors come back as `{"ok":false,"error":"..."}`. Requests execute on the
editor's main thread; a busy editor answers within 15 s or returns a timeout
error. One client connection at a time.

## Prompt block for the agent

> The XFined Editor (S.T.A.L.K.E.R. X-Ray SDK level editor) is running locally
> with an MCP endpoint. Use `xfined_state` to learn which project and scene are
> open, and `xfined_screenshot_editor` / `xfined_screenshot_viewport` to see
> the editor with your own eyes before and after any change you make. The
> screenshots work even when the editor window is covered — always verify the
> actual UI state visually instead of assuming. `xfined_screenshot_editor` arms
> the capture on its first call and returns the image on the next one, so a
> "capture just armed" answer means call it again, not that it failed.
>
> If you start the editor yourself, launch it as
> `LevelEditor.exe -project "<project folder>"` so it opens straight into that
> project instead of waiting on the project picker. On a running editor,
> `xfined_open_project` switches projects.
>
> The project folder is the only writable place. The shared SDK library
> ("Editor Content") and the linked game install ("DARF Content") are read-only
> sources: pull assets out of them with `xfined_content_browser_copy`, which
> puts each one where the engine expects it.
