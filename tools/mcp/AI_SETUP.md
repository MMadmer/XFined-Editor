# XFined Editor — MCP setup for AI agents

Copy-paste this file (or its relevant part) into any AI assistant that supports
MCP. It contains everything needed to connect to and control the editor.

## What is available

The XFined Editor hosts a local automation endpoint whenever it is running:

* transport: TCP, line-based JSON, `127.0.0.1:28016`
* alive from the very first frame — the project browser stage included
* an MCP stdio bridge is bundled at `tools/mcp/xfined_mcp.py` (Python 3, stdlib only)

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
| `xfined_screenshot_editor` | PNG image of the whole editor window (all panels); captured via PrintWindow, so it works even when the editor is covered by other windows |
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
| `xfined_scene_tree` | the World Outliner's data: groups per object tool class sorted by class name, each with `count`/`total` and `objects` (name, selected, visible); filters `filter`, `class`, `limit` (default 200) |
| `xfined_undo` | undo the last scene operation |
| `xfined_redo` | redo the last undone scene operation |

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
> actual UI state visually instead of assuming.
