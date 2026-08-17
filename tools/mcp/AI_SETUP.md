# XFined Editor — MCP setup for AI agents

Copy-paste this file (or its relevant part) into any AI assistant that supports
MCP. It contains everything needed to connect to and control the editor.

## What is available

The XFined Editor hosts a local automation endpoint whenever it is running:

* transport: TCP, line-based JSON, `127.0.0.1:28016`
* alive from the very first frame — the project browser stage included
* an MCP stdio bridge is bundled at `tools/mcp/xfined_mcp.py` (Python 3, stdlib only)

Scene-load performance can be checked against the same endpoint without an MCP
client. Keep the expected object count in the command so a faster incomplete
load fails the benchmark instead of looking like an improvement:

```powershell
python tools/mcp/benchmark_scene_load.py l01_escape.level --warmup 1 --repeats 5 --expected-total 6002
```

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
| `xfined_mod_set_manifest` | update mod.ltx fields: `id`/`name`/`version`/`mode` strings, `provides_mode_id`+`provides_mode_title` (declare a game mode, empty id removes — the title is what the player reads, any language), `target` (`default`/`existing`/`new`, inferred from the mode fields when omitted, but "the ordinary game" must be said out loud), `requires`/`after`/`before`/`conflicts` comma-joined lists |
| `xfined_mod_export` | export the project to `<target>/modules/<id>/` as an XMS module (no `target` = deploy to the linked game's `modules/`; **not** `mods/` — that is JSGME's folder, and a module sitting there is one JSGME offers to copy over the game); a **clean build** — the target module folder is emptied first, so nothing a previous build left can survive; refuses targets inside the game's `gamedata`; `flat=true` for a plain `gamedata_<id>` overlay + compatibility report. A module that declares a mode also gets `patch/xms_modes.xmlp`, `gamedata/configs/text/rus/<id>_modes.xml` and `scripts/mode_register.script` written for it — the checkbox that puts the mode on the new-game screen, measured against the layout the linked game really ships |
| `xfined_mod_export_spawn` | export placed objects as an additive spawn layer `spawn/<level>.xspawn` (`op=add` + section + world transform + visual override) — the composable way to add things to a base level; only AUTHORED objects ship (see `xfined_mark_authored`); a project-local model is staged into `gamedata/meshes/`, a model found nowhere skips its op instead of shipping a crash; args `level`, `selected_only`, `section`, `mode`. `xfined_mod_export` runs this bake automatically for the open scene (mirror semantics: the scene is the level file's source of truth) |
| `xfined_mark_authored` | flip the mod-content flag on the selection (`authored`, default true) — the build ships only authored objects; an imported base level's objects stay base and never export |
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
| `xfined_list_spawn_sections` | the gameplay-object roster: every config section of the **linked game** with `$spawn` — actors, NPC, items, restrictors/triggers, anomalies; `filter` substring, `limit`; feeds `place_spawn` |
| `xfined_place_spawn` | place a gameplay object by config `section` (spawn element, like the old SDK's Spawn Element mode): server entity from the section, declared shapes attached, properties (logic/custom_data) editable in Properties; optional `x`/`y`/`z`, `snap_to_ground` |
| `xfined_view_mode` | read/set viewport rendering: `preset` (`unlit` = normal textured view, `lit` = 8-slot fixed-function lighting so most geometry goes black, `wireframe`, `point`), `fill`, `shade`, and the raw `lighting`/`textures`/`edged_faces`/`filter_linear`/`fog`/`environment`/`grid`/`safe_rect` toggles |
| `xfined_list_assets` | list available assets: `category` (Objects by default), `filter`, `limit`; names feed `place_object`'s `ref` |
| `xfined_asset_preview` | PNG thumbnail of one asset (`name`, optional `category`) for judging its look before placing |
| `xfined_rename_object` | rename a scene object (`name` -> `new_name`, must be unique) |
| `xfined_delete_selected` | delete every selected object; returns the removed count; undoable |
| `xfined_save_scene` | save the opened scene (its current file, or an explicit `file`) |
| `xfined_outliner_show` | open (`open=true`, default) or close the World Outliner panel |
| `xfined_game_modes` | game modes a module can target, scanned from the LINKED game: campaigns only (rule options like "one life" are excluded, so are checkboxes the game comments out), each with `id`, `caption` (what the player sees), `key` and `source`. A module's target is set in the manifest and the module export refuses to run without one |
| `xfined_outliner_filter` | drive the outliner's search box and type funnel: `text` (Unreal grammar - every word must match, `-word` excludes, `"two words"` match together), `types` (`;`-separated classes to show, empty shows all), `selected_only`; returns `shown`/`total` |
| `xfined_scene_tree` | the World Outliner's data: groups per object tool class sorted by class name, each with `count`/`total` and `objects` (name, selected, visible); filters `filter`, `class`, `limit` (default 200) |
| `xfined_content_browser_open` | reveal an asset **or a folder** in the Content Browser: opens the panel, switches `source` (`project`/`editor`/`darf`), navigates there and selects it; a folder path (`levels`, `levels\l07_military`) is how the grid is walked into a folder without a mouse; `open=true` also opens an asset's viewer (the double-click action) |
| `xfined_content_browser_selection` | what the browser shows right now: open flag, active source, current folder, selected assets |
| `xfined_content_browser_copy` | copy assets out of a READ-ONLY source into the project: `names` (`;`-separated) or `folder` (recursive), optional `source`/`category`, `overwrite` (default true). With `dst` they land in that project-relative folder, a folder keeping its own name and subtree — the same clipboard and paste the browser's menu uses. Without `dst` each asset is mirrored where the engine expects it instead: an Objects entry in `<project>/rawdata/objects/...`, a mesh in `<project>/gamedata/meshes/...`, a texture with its `.thm` |
| `xfined_content_copy` / `xfined_content_move` | copy/move files or folders by path; `src` accepts a `;`-separated list, `dst` is a folder inside the project, `recursive`/`overwrite`. Moving out of a read-only source is refused — a move deletes its source |
| `xfined_content_delete` | delete files/folders inside the project; `recursive` defaults to false so a non-empty folder is reported instead of wiped |
| `xfined_content_mkdir` | create a folder (with missing parents) inside the project |
| `xfined_list_commands` | the editor's own command registry: every action the menus and shortcuts can perform, as `{id, name, menu, presets}` — the editor's full control surface, and commands added in future builds appear here automatically |
| `xfined_exec_command` | execute ANY registry command — the same dispatch menus and shortcuts use. Address by `id`, `COMMAND_*` `name` or menu path; `p1s`/`p2s` pass strings, `p1i`/`p2i` integers. Prefer a dedicated tool when one exists; this is the escape hatch for everything else |
| `xfined_theme` | read (`action=get`), persist (`action=set`, `preset=xfined-purple` or `graphite`) or reset the live editor theme; omitting `action` reads it |
| `xfined_command_palette` | query the ranked Ctrl+Shift+P catalog, open/close the palette, inspect its state, or execute an exact returned command/subcommand pair |
| `xfined_viewport_navigation` | read/control the viewport orientation widget: six canonical axis views, perspective reset, and frame all/selection |
| `xfined_progress` | read active nested editor tasks with percentage/detail/elapsed time, or request cooperative cancellation for tasks that yield UI frames; blocking legacy jobs keep live feedback in the progress console |
| `xfined_property_inspector` | search/filter the selection or world property tree, expand/collapse all, and open/close the corresponding panel |
| `xfined_content_browser_navigation` | inspect or drive Content Browser Back/Forward/Up/Home, source/category/folder breadcrumbs and process-session Favorites |
| `xfined_undo` | undo the last scene operation |
| `xfined_redo` | redo the last undone scene operation |
| `xfined_quest_catalog` | the quest kind catalog (NQ, see below): every trigger / main action / extra action / condition kind with `group`, `title`, `desc`, `use`, `params` (name, type, required, default, min/max, enum), `pins`, `wait`/`once`, `event`; plus catalog `version` and `source` (`game` = read from the linked install, `bundled` = the editor's fallback copy). Read it before writing a quest |
| `xfined_quest_list` | every `*.nqasset` of the project: path, id, title, node count, errors/warnings, open/dirty (unparsable files: `readable:false` + the parse error) |
| `xfined_quest_new` | create a quest from the minimal template (`trigger.start -> flow.end`, id = file name) at a project-relative `path` and open it; refuses to overwrite |
| `xfined_quest_open` / `xfined_quest_close` | open a quest document (validates, lays out nodes without `pos`) / close it (`discard:true` drops unsaved edits, otherwise a dirty document answers `unsaved`) |
| `xfined_quest_get` | the quest as text: `lua` (canonical file text), `outline` (one line per node in flow order + the problems), `problems` `[{code,severity,node,slot,message}]`, counts. Works for files that are not open |
| `xfined_quest_write` | replace a quest with the whole file text `lua`. Open document: replaced in memory with an undo snapshot (`saved:false`, then `xfined_quest_save`). Closed or new file: written to disk canonically when it parses (`saved:true`). Validation problems are returned but never block the write - only the build refuses errors |
| `xfined_quest_apply` | point edits: `ops` = Lua text of a list of operations (`add_node`, `set_node`, `rename_node`, `remove_node`, `connect`, `disconnect`, `add_action`, `set_action`, `move_action`, `remove_action`, `set_quest`, `set_pos`; 1-based indexes), all-or-nothing, one undo step; opens the document when needed and leaves it dirty |
| `xfined_quest_undo` / `xfined_quest_redo` | the quest document's own undo ring (200 steps; the scene undo never touches quests) |
| `xfined_quest_validate` | problems of a quest (open document or file): `E001-E050` block the build, `W011-W070` warn |
| `xfined_quest_save` / `xfined_quest_reload` | write the open document canonically (`force:true` overrides "modified externally") / re-read it from disk |
| `xfined_quest_layout` | deterministic top-down auto layout (`all:false` = only nodes without `pos`); returns the positions |
| `xfined_quest_view` | open/focus the graph tab and set the view (`frame:"all"|<node>`, `zoom_level` 0..9, `cx`/`cy` which override `frame`, `slot:"enter:0"`/`"exit:1"` to open that action of the framed node in the inspector, `"none"` to clear); answers with the resulting `zoom_level`/`center`/`selected`/`slot`; follow with `xfined_screenshot_editor` |
| `xfined_quest_lookup` | search picker data: `type` = `item_section`, `squad_section`, `level`, `smart`, `story_id`, `profile`, `community`, `info`, `spot_type` (from the linked game) or `task_id`, `var_name`, `ref_name`, `node_id` (need `path`), `quest_id` (project quests); `query` substring, `limit` |
| `xfined_quest_check_all` | the build gate on its own: validates every quest of the project exactly like `xfined_mod_export` does (`passed`, counts, log lines) |

Everything that writes is clamped to the project folder: the linked game install
and the shared SDK library are sources you copy **out of**, never into.

## Quest workflow for AI (NQ quest graphs)

A quest is one `.nqasset` file = declarative Lua (`return { nq = 1, id = ...,
title = ..., activation = "auto", vars = {...}, tasks = {...}, nodes = {...} }`)
that the game interprets at runtime; the editor never compiles it. Everything is
text, no screenshots needed: coordinates (`pos`) are optional, problems come
back as codes, `xfined_quest_get` returns the canonical file plus an outline of
the graph. Full contract: `docs/NQ_ARCHITECTURE.md` (§4 format, §6 kinds, §15
codes), examples: `docs/nq/examples/*.nqasset`.

1. `xfined_quest_catalog` — learn the kinds: node kinds (`use` main/trigger)
   with their `params` and `pins`, extra actions for `on_enter`/`on_exit`,
   conditions for `cond` (`event = true` ones only in `trigger.when` /
   `wait.when` / `wait.any`).
2. `xfined_quest_write` with the whole file (or `xfined_quest_new` + `xfined_quest_apply`).
   Ids are `[a-z0-9_]`; a node is `{ id, kind, params, cond, on_enter, on_exit, out = { pin = "id" | { "id", ... } } }`;
   phrases (`dialog.npc_phrase` / `dialog.actor_phrase`) hang off a `dialog.topic`
   through `next` and must alternate speakers; refs to things the quest spawns
   are `{ ref = "name" }`, world objects `{ story = "story_id" }` (look them up
   with `xfined_quest_lookup`).
3. Read the answer: `errors` must be 0 (`W`-codes are hints: `W060` = value not
   found in the linked game, `W011` = a phrase with only conditional replies).
   Fix and write again; `xfined_quest_get` shows the canonical text and outline.
4. `xfined_quest_check_all`, then `xfined_mod_export` — the export refuses with
   `quest graph errors: N (see log)` while any quest has an `E` problem; the
   `.nqasset` files ship into `<game>\modules\<id>\` unchanged.

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
editor's main thread; a busy editor can take up to three minutes before the
endpoint returns a timeout. The server accepts up to 16 concurrent clients and
queues at most 64 main-thread requests; each request buffer is capped at 1 MiB.

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
>
> Quests are `.nqasset` files (declarative Lua graphs, see the "Quest workflow"
> section): `xfined_quest_catalog` for the kinds, `xfined_quest_write` /
> `xfined_quest_apply` to author, `xfined_quest_get` for the canonical text and
> outline, `xfined_quest_validate` / `xfined_quest_check_all` before
> `xfined_mod_export`. Never draw the graph from screenshots - the outline and
> the problem codes are the working view.
